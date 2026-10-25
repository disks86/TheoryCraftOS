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

// Host-side tests for the input and display controls: Slider, LevelMeter, CheckBox, RadioButton,
// Toggle, ColorPicker and DatePicker, plus the colour and date helpers they are built on.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Gui/Color.hpp"
#include "Gui/Controls/BarChart.hpp"
#include "Gui/Controls/CheckBox.hpp"
#include "Gui/Controls/ColorPicker.hpp"
#include "Gui/Controls/DatePicker.hpp"
#include "Gui/Controls/HeatMap.hpp"
#include "Gui/Controls/LevelMeter.hpp"
#include "Gui/Controls/Knob.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/PieChart.hpp"
#include "Gui/Controls/RadioButton.hpp"
#include "Gui/Controls/ScrollPanel.hpp"
#include "Gui/Controls/Slider.hpp"
#include "Gui/Controls/Toggle.hpp"
#include "Gui/Controls/WordCloud.hpp"
#include "Gui/Controls/ToastCenter.hpp"
#include "Gui/Controls/Tooltip.hpp"
#include "Gui/Date.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/AllocatedView.hpp"

using namespace gui;
namespace key = hardware::key;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)
static bool near(int32_t a, int32_t b, int32_t tolerance) { return a >= b - tolerance && a <= b + tolerance; }

struct Screen {
  TestAllocator& a;
  graphics::Surface surface;
  graphics::SoftwareRenderer2D renderer;
  Screen(TestAllocator& alloc, uint32_t w, uint32_t h) : a(alloc), surface(make(alloc, w, h)), renderer(surface) {}
  ~Screen() { release_view(a, surface.pixels); }
  static graphics::Surface make(TestAllocator& a, uint32_t w, uint32_t h) {
    graphics::Surface s;
    s.pixels = allocate_view<uint32_t>(a, w * h);
    s.width = w; s.height = h; s.pitch = w;
    for (uint32_t& p : s.pixels) p = 0;
    return s;
  }
  uint32_t px(int32_t x, int32_t y) const { return surface.row(y)[x]; }
};

static void press(Window& w, uint8_t usage, uint8_t modifiers = 0) {
  KeyArgs k;
  k.usage = usage; k.modifiers = modifiers; k.character = hardware::key_to_ascii(usage, modifiers);
  w.key(k);
}
static void click(Window& w, int32_t x, int32_t y) {
  w.mouse_move(x, y);
  w.mouse_button(x, y, MouseButton::Left, true);
  w.mouse_button(x, y, MouseButton::Left, false);
}
static Point center(Control& c) {
  const Point o = c.window_origin();
  return {o.x + static_cast<int32_t>(c.bounds().width) / 2, o.y + static_cast<int32_t>(c.bounds().height) / 2};
}
static void click_center(Window& w, Control& c) { const Point p = center(c); click(w, p.x, p.y); }

struct Counter {
  int count = 0;
  Control* last = nullptr;
  static void fn(void* context, Control& sender) { auto* c = static_cast<Counter*>(context); ++c->count; c->last = &sender; }
  Handler handler() { return {&Counter::fn, this}; }
};

static void test_helpers() {
  static_assert(weekday({1970, 1, 1}) == 4);   // a Thursday
  static_assert(weekday({2000, 1, 1}) == 6);
  static_assert(weekday({2024, 2, 29}) == 4);
  static_assert(days_in_month(1900, 2) == 28 && days_in_month(2000, 2) == 29 && days_in_month(2024, 2) == 29);
  static_assert(is_valid({2024, 2, 29}) && !is_valid({2023, 2, 29}) && !is_valid({2024, 13, 1}) && !is_valid({0, 1, 1}));
  static_assert(date_from_days(days_from_date({2026, 10, 17})) == Date{2026, 10, 17});
  static_assert(days_from_date({1970, 1, 2}) == 1 && days_from_date({1969, 12, 31}) == -1);
  static_assert(add_days({2024, 2, 28}, 2) == Date{2024, 3, 1} && add_days({2024, 3, 1}, -1) == Date{2024, 2, 29});
  static_assert(add_days({9999, 12, 31}, 1) == Date{9999, 12, 31} && add_days({1, 1, 1}, -1) == Date{1, 1, 1});
  static_assert(add_months({2024, 1, 31}, 1) == Date{2024, 2, 29} && add_months({2024, 12, 15}, 1) == Date{2025, 1, 15});
  static_assert(add_months({2024, 1, 15}, -1) == Date{2023, 12, 15});
  char text[11];
  format_date({2026, 3, 9}, text);
  CHECK(strcmp(text, "2026-03-09") == 0);

  static_assert(hsv_to_color({0, 255, 255}) == 0xFFFF0000 && hsv_to_color({120, 255, 255}) == 0xFF00FF00 &&
                hsv_to_color({240, 255, 255}) == 0xFF0000FF && hsv_to_color({0, 0, 255}) == 0xFFFFFFFF &&
                hsv_to_color({77, 200, 0}) == 0xFF000000);
  static_assert(color_to_hsv(0xFFFF0000).hue == 0 && color_to_hsv(0xFF00FF00).hue == 120 && color_to_hsv(0xFF0000FF).hue == 240 &&
                color_to_hsv(0xFF808080).saturation == 0);
  int bad = 0;
  for (uint32_t c = 0; c < 0x1000000; c += 0x010305) {
    const uint32_t original = 0xFF000000 | c, back = hsv_to_color(color_to_hsv(original));
    const int d = abs(int(red_of(original)) - red_of(back)) + abs(int(green_of(original)) - green_of(back)) + abs(int(blue_of(original)) - blue_of(back));
    if (d > 6) ++bad;
  }
  CHECK(bad == 0);
  format_color(0xFF1A2B3C, text);
  CHECK(strcmp(text, "#1A2B3C") == 0);
  CHECK(contrasting_text_color(0xFFFFFFFF) == 0xFF000000 && contrasting_text_color(0xFF000080) == 0xFFFFFFFF);
}

static void test_slider() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 300);
    Window w(a, screen.renderer, theme, {300, 300});
    Slider s(a);
    s.set_bounds({10, 10, 120, 24});
    w.add(&s);
    Counter changes;
    s.on_value_changed(changes.handler());
    CHECK(s.minimum() == 0 && s.maximum() == 100 && s.value() == 0);
    CHECK(s.thumb_rect().x == 0 && s.thumb_rect().width == 11);
    s.set_value(100);
    CHECK(s.thumb_rect().x == 109 && changes.count == 1);
    s.set_value(500);  // clamped, no change
    CHECK(s.value() == 100 && changes.count == 1);
    s.set_value(50);
    CHECK(near(s.thumb_rect().x, 54, 1));
    s.set_value(0);
    changes.count = 0;

    // A click on the track centres the thumb there and starts a drag.
    w.mouse_move(10 + 80, 22);
    w.mouse_button(10 + 80, 22, MouseButton::Left, true);
    CHECK(near(s.value(), 69, 1) && changes.count == 1 && w.focus() == &s);
    w.mouse_move(10 + 60, 22);
    CHECK(near(s.value(), 51, 2));
    w.mouse_move(10 - 40, 80);  // dragging far outside clamps
    CHECK(s.value() == 0);
    w.mouse_move(10 + 400, 22);
    CHECK(s.value() == 100);
    w.mouse_button(10 + 400, 22, MouseButton::Left, false);
    w.mouse_move(10 + 10, 22);  // no longer dragging
    CHECK(s.value() == 100);

    // Picking up the thumb keeps the grab point under the pointer.
    s.set_value(50);
    const int32_t thumb_x = 10 + s.thumb_rect().x;
    w.mouse_move(thumb_x + 3, 22);
    w.mouse_button(thumb_x + 3, 22, MouseButton::Left, true);
    CHECK(s.value() == 50);  // no jump
    w.mouse_move(thumb_x + 3 + 10, 22);
    CHECK(near(s.value(), 50 + 10 * 100 / 109, 1));
    w.mouse_button(thumb_x + 13, 22, MouseButton::Left, false);

    // Keys and the wheel.
    s.set_value(40);
    s.set_step(2, 25);
    press(w, key::kRight);
    CHECK(s.value() == 42);
    press(w, key::kLeft);
    press(w, key::kLeft);
    CHECK(s.value() == 38);
    press(w, 0x4B);  // Page Up
    CHECK(s.value() == 63);
    press(w, 0x4E);
    press(w, 0x4E);
    press(w, 0x4E);
    CHECK(s.value() == 0);
    press(w, 0x4D);  // End
    CHECK(s.value() == 100);
    press(w, 0x4A);  // Home
    CHECK(s.value() == 0);
    w.mouse_wheel(20, 20, 1);
    CHECK(s.value() == 2);
    w.mouse_wheel(20, 20, -1);
    w.mouse_wheel(20, 20, -1);
    CHECK(s.value() == 0);

    // A new range keeps the value inside it; a backwards range collapses.
    s.set_value(80);
    s.set_range(10, 20);
    CHECK(s.value() == 20 && s.minimum() == 10);
    s.set_range(30, 5);
    CHECK(s.minimum() == 30 && s.maximum() == 30 && s.value() == 30);
    s.set_range(0, 10);
    s.set_value(10);
    CHECK(s.thumb_rect().x == 109);

    // Vertical: the maximum is at the top.
    Slider v(a, Orientation::Vertical);
    v.set_bounds({150, 10, 24, 120});
    w.add(&v);
    CHECK(v.thumb_rect().y == 109 && v.thumb_rect().height == 11);
    v.set_value(100);
    CHECK(v.thumb_rect().y == 0);
    w.mouse_move(160, 10 + 109);   // near the bottom: a low value
    w.mouse_button(160, 10 + 109, MouseButton::Left, true);
    CHECK(v.value() < 15);
    w.mouse_button(160, 10 + 109, MouseButton::Left, false);

    // Disabled: no input.
    s.set_value(5);
    s.set_enabled(false);
    click(w, 10 + 100, 22);
    CHECK(s.value() == 5);
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_knob() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 300);
    Window w(a, screen.renderer, theme, {300, 300});
    Knob k(a);
    k.set_bounds({10, 10, 48, 48});
    w.add(&k);
    Counter changes;
    k.on_value_changed(changes.handler());
    CHECK(k.minimum() == 0 && k.maximum() == 100 && k.value() == 0 && k.radius() == 21);

    // Fixed-point sine, for any angle.
    CHECK(Knob::sine(0) == 0 && Knob::sine(90) == 1024 && Knob::sine(180) == 0 && Knob::sine(270) == -1024 && Knob::sine(-90) == -1024);
    CHECK(Knob::sine(30) == 512 && Knob::sine(150) == 512 && Knob::sine(-30) == -512 && Knob::sine(450) == 1024 && near(Knob::sine(47), 749, 2));

    // The pointer sweeps -135 .. 135 degrees.
    CHECK(k.angle() == -135 && k.angle_of(100) == 135 && k.angle_of(50) == 0 && k.angle_of(500) == 135 && k.angle_of(-5) == -135);
    k.set_value(50);
    const Point c = k.center(), up = k.point_at(0, 1000);
    CHECK(c.x == 24 && c.y == 24 && up.x == 24 && up.y == 3);
    k.set_value(100);
    CHECK(k.point_at(k.angle(), 1000).x > c.x && k.point_at(k.angle(), 1000).y > c.y);   // 4:30: right and below
    k.set_value(500);   // clamped, no change
    CHECK(k.value() == 100 && changes.count == 2);
    k.set_value(0);
    w.render();
    const Point tip = k.point_at(-135, 720);
    CHECK(screen.px(10 + tip.x, 10 + tip.y) == theme.text);   // the pointer reaches its tip

    // Dragging up (or right) turns it up, down (or left) turns it down; 150 pixels cover the range.
    changes.count = 0;
    w.mouse_move(10 + 24, 10 + 24);
    w.mouse_button(10 + 24, 10 + 24, MouseButton::Left, true);
    CHECK(k.dragging() && k.value() == 0 && w.focus() == &k);
    w.mouse_move(10 + 24, 10 + 24 - 75);
    CHECK(k.value() == 50 && changes.count == 1);
    w.mouse_move(10 + 24 + 30, 10 + 24 - 75);
    CHECK(k.value() == 70);
    w.mouse_move(10 + 24, 10 + 24 + 400);   // far down: clamped
    CHECK(k.value() == 0);
    w.mouse_move(10 + 24, 10 + 24 - 400);
    CHECK(k.value() == 100);
    w.mouse_button(10 + 24, 10 + 24 - 400, MouseButton::Left, false);
    CHECK(!k.dragging());
    w.mouse_move(10 + 24, 10 + 24);
    CHECK(k.value() == 100);   // no longer dragging

    // Keys and the wheel.
    k.set_value(40);
    k.set_step(2, 25);
    press(w, key::kUp);
    CHECK(k.value() == 42);
    press(w, key::kLeft);
    press(w, key::kDown);
    CHECK(k.value() == 38);
    press(w, 0x4B);
    CHECK(k.value() == 63);
    press(w, 0x4E);
    press(w, 0x4E);
    press(w, 0x4E);
    CHECK(k.value() == 0);
    press(w, 0x4D);
    CHECK(k.value() == 100);
    press(w, 0x4A);
    CHECK(k.value() == 0);
    w.mouse_wheel(20, 20, 1);
    CHECK(k.value() == 2);
    w.mouse_wheel(20, 20, -1);
    w.mouse_wheel(20, 20, -1);
    CHECK(k.value() == 0);

    // A new range keeps the value inside it; a backwards range collapses and the pointer rests at the start.
    k.set_value(80);
    k.set_range(10, 20);
    CHECK(k.value() == 20 && k.minimum() == 10 && k.angle() == 135);
    k.set_range(30, 5);
    CHECK(k.minimum() == 30 && k.maximum() == 30 && k.value() == 30 && k.angle() == -135);
    k.set_range(-50, 50);
    k.set_value(0);
    CHECK(k.angle() == 0);
    k.set_range(INT32_MIN, INT32_MAX);   // no overflow in the angle or the drag
    k.set_value(INT32_MAX);
    CHECK(k.angle() == 135);
    w.mouse_move(34, 34);
    w.mouse_button(34, 34, MouseButton::Left, true);
    w.mouse_move(34, 34 - 140);
    w.mouse_button(34, 34 - 140, MouseButton::Left, false);
    CHECK(k.value() == INT32_MAX);

    // Too small to draw, and disabled: nothing is drawn, no input.
    k.set_range(0, 100);
    k.set_value(5);
    k.set_bounds({10, 10, 6, 6});
    CHECK(k.radius() == 0);
    w.render();
    k.set_bounds({10, 10, 48, 48});
    k.set_enabled(false);
    click(w, 10 + 24, 10 + 24);
    press(w, key::kUp);
    CHECK(k.value() == 5);
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_level_meter() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 150);
    Window w(a, screen.renderer, theme, {300, 150});
    LevelMeter m(a);
    m.set_bounds({10, 10, 120, 18});
    w.add(&m);
    CHECK(m.segment_count() == 14);
    CHECK(m.lit_segments(0) == 0 && m.lit_segments(1) == 1 && m.lit_segments(50) == 7 && m.lit_segments(100) == 14);
    // Zones: green to 70, yellow to 90, red above - until set_zones() says otherwise.
    CHECK(m.zone_at(0) == MeterZone::Green && m.zone_at(70) == MeterZone::Green && m.zone_at(71) == MeterZone::Yellow && m.zone_at(90) == MeterZone::Yellow && m.zone_at(91) == MeterZone::Red);
    m.set_zones(ZoneDirection::LowIsBad, 15, 40);   // a battery
    CHECK(m.zone_at(0) == MeterZone::Red && m.zone_at(15) == MeterZone::Red && m.zone_at(16) == MeterZone::Yellow && m.zone_at(40) == MeterZone::Yellow && m.zone_at(41) == MeterZone::Green && m.zone_at(100) == MeterZone::Green);
    m.set_zones(ZoneDirection::HighIsBad, 85, 70);  // swapped breaks are put in order
    CHECK(m.zone_at(70) == MeterZone::Green && m.zone_at(80) == MeterZone::Yellow && m.zone_at(86) == MeterZone::Red);
    m.use_default_zones();
    CHECK(m.zone_direction() == ZoneDirection::HighIsBad && m.zone_at(71) == MeterZone::Yellow);

    m.set_level(50);
    w.render();
    CHECK(screen.px(10 + 4, 10 + 8) == 0xFF00C000);              // first segment: lit green
    CHECK(screen.px(10 + 2 + 6 * 8 + 3, 10 + 8) == 0xFF00C000);  // seventh: lit
    CHECK(screen.px(10 + 2 + 7 * 8 + 3, 10 + 8) == 0xFF004000);  // eighth: dim green
    CHECK(screen.px(10 + 2 + 13 * 8 + 3, 10 + 8) == 0xFF400000); // last: dim red

    // The peak stays marked after the level falls.
    m.set_level(80);
    m.set_level(20);
    CHECK(m.level() == 20 && m.peak() == 80);
    w.render();
    CHECK(screen.px(10 + 2 + 11 * 8 + 3, 10 + 8) == 0xFFE0E000);  // the 12th segment holds the peak (yellow)
    CHECK(screen.px(10 + 2 + 8 * 8 + 3, 10 + 8) == 0xFF004000);   // between level and peak: dark
    m.reset_peak();
    CHECK(m.peak() == 20);
    m.set_peak_hold(false);
    m.set_level(60);
    m.set_level(10);
    CHECK(m.peak() == 10);

    // Out of range values are clamped; the range can change.
    m.set_level(1000);
    CHECK(m.level() == 100);
    m.set_range(0, 50);
    CHECK(m.level() == 50 && m.lit_segments(25) == 7);
    m.set_range(-60, 0);
    m.set_level(-30);
    CHECK(m.lit_segments(-30) == 7 && m.lit_segments(-60) == 0);

    // Vertical meters fill upwards.
    LevelMeter v(a, Orientation::Vertical);
    v.set_bounds({200, 10, 18, 120});
    w.add(&v);
    v.set_level(30);
    w.render();
    CHECK(screen.px(200 + 8, 10 + 120 - 2 - 3) == 0xFF00C000);   // bottom segment lit
    CHECK(screen.px(200 + 8, 10 + 8) == 0xFF400000);             // top segment dark
  }
  CHECK(a.live == 0);
}

static void test_pie_chart() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 200);
    Window w(a, screen.renderer, theme, {300, 200});
    PieChart p(a);
    p.set_bounds({10, 10, 200, 140});
    p.set_legend(false);
    w.add(&p);
    CHECK(p.slice_count() == 0 && p.slice_at(100, 70) == -1 && p.total() == 0);
    CHECK(p.add_slice("A", 25) == 0 && p.add_slice("B", 25) == 1 && p.add_slice("C", 50, 0xFF112233) == 2);
    CHECK(p.total() == 100 && p.permille(0) == 250 && p.permille(2) == 500 && p.slice_color(2) == 0xFF112233 && p.slice_color(0) != 0);
    // Slices start at 12 o'clock and run clockwise: A the first quarter, B the second, C the half.
    CHECK(p.slice_at_angle(0) == 0 && p.slice_at_angle(16383) == 0 && p.slice_at_angle(16385) == 1 && p.slice_at_angle(32770) == 2 && p.slice_at_angle(65535) == 2);
    const Rect2D r = p.pie_rect();
    CHECK(r.width == 132 && r.height == 132);
    const int32_t cx = r.x + 66, cy = r.y + 66;
    CHECK(p.slice_at(cx + 30, cy - 30) == 0);   // upper right
    CHECK(p.slice_at(cx + 30, cy + 30) == 1);   // lower right
    CHECK(p.slice_at(cx - 30, cy + 30) == 2);   // lower left
    CHECK(p.slice_at(cx - 30, cy - 30) == 2);   // upper left
    CHECK(p.slice_at(r.x + 1, r.y + 1) == -1);  // the corner of the square is outside the disc
    w.render();
    CHECK(screen.px(10 + cx + 30, 10 + cy - 30) == p.slice_color(0));
    CHECK(screen.px(10 + cx - 30, 10 + cy + 30) == 0xFF112233);
    CHECK(screen.px(10 + r.x + 1, 10 + r.y + 1) == theme.face);

    // Selection by click and by key; the handler fires only on a change.
    click(w, 10 + cx + 30, 10 + cy + 30);
    CHECK(p.selected_index() == 1);
    w.render();
    CHECK(screen.px(10 + cx + 30, 10 + cy + 30) != p.slice_color(1));   // drawn lighter
    click(w, 10 + 2, 10 + 2);
    CHECK(p.selected_index() == -1);
    press(w, key::kDown);
    CHECK(p.selected_index() == 0);
    press(w, key::kUp);
    CHECK(p.selected_index() == 2);

    // Zero valued slices take no angle; an all zero chart is an empty disc.
    p.set_slice_value(1, 0);
    CHECK(p.slice_at_angle(30000) == 2 && p.permille(1) == 0);
    p.set_slice_value(0, 0);
    p.set_slice_value(2, 0);
    CHECK(p.slice_at(cx, cy) == -1 && p.permille(2) == 0);
    w.render();
    CHECK(screen.px(10 + cx + 30, 10 + cy - 30) == theme.field);

    // Huge values do not overflow.
    p.set_slice_value(0, UINT64_MAX);
    p.set_slice_value(1, UINT64_MAX);
    CHECK(p.total() == UINT64_MAX && p.slice_at_angle(0) == 0);
    p.clear_slices();
    CHECK(p.slice_count() == 0 && p.selected_index() == -1);

    // The legend shows swatch, label and share, and a click on its row selects the slice.
    p.set_legend(true);
    p.add_slice("Alpha", 1);
    p.add_slice("Beta", 3);
    const int32_t row = p.legend_row_at(200 - 10, 70 - 18 / 2 * 2 + 18 + 5);
    CHECK(row == 1 || row == 0);
    p.set_selected_index(0);
    w.render();
    CHECK(p.pie_rect().width < 132);
    p.set_selected_index(9);
    CHECK(p.selected_index() == -1);
  }
  CHECK(a.live == 0);
}

static void test_bar_chart() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 200);
    Window w(a, screen.renderer, theme, {300, 200});
    BarChart b(a);
    b.set_bounds({10, 10, 260, 160});
    w.add(&b);
    CHECK(b.bar_count() == 0 && b.bar_at(100, 50) == -1 && b.scale_top() == 1);
    CHECK(b.add_bar("A", 10) == 0 && b.add_bar("B", 20, 0xFF112233) == 1 && b.add_bar("C", 0) == 2 && b.add_bar("D", 1) == 3);
    CHECK(b.scale_top() == 20 && b.bar_color(1) == 0xFF112233 && b.bar_color(0) != 0);
    const Rect2D p = b.plot_rect();
    CHECK(p.width > 0 && p.height > 0);
    // Heights are in proportion to the scale; any non-zero value shows at least a pixel.
    CHECK(b.bar_height(20) == p.height && b.bar_height(10) == p.height / 2 && b.bar_height(0) == 0 && b.bar_height(1) >= 1);
    b.set_maximum(40);
    CHECK(b.bar_height(20) == p.height / 2 && b.bar_height(400) == p.height);   // above a fixed maximum: full height
    b.set_maximum(0);

    // The columns tile the plot; a pixel is in exactly one of them.
    int32_t covered = 0;
    for (int32_t i = 0; i < 4; ++i) covered += static_cast<int32_t>(b.column_rect(i).width);
    CHECK(covered == static_cast<int32_t>(p.width));
    for (int32_t i = 0; i < 4; ++i) {
      const Rect2D c = b.column_rect(i);
      CHECK(b.bar_at(c.x, c.y + 3) == i && b.bar_at(c.x + static_cast<int32_t>(c.width) - 1, c.y + 3) == i);
    }
    CHECK(b.bar_at(p.x - 1, p.y + 3) == -1 && b.bar_at(p.x + 3, p.y - 1) == -1);

    w.render();
    const Rect2D c1 = b.column_rect(1);
    CHECK(screen.px(10 + c1.x + static_cast<int32_t>(c1.width) / 2, 10 + p.y + static_cast<int32_t>(p.height) - 5) == 0xFF112233);   // inside the tall bar
    const Rect2D c2 = b.column_rect(2);
    CHECK(screen.px(10 + c2.x + static_cast<int32_t>(c2.width) / 2, 10 + p.y + static_cast<int32_t>(p.height) - 5) == theme.field);  // a zero bar is empty

    // Selection by click and by key; the handler fires only on a change.
    click(w, 10 + c1.x + 3, 10 + p.y + 3);
    CHECK(b.selected_index() == 1);
    w.render();
    CHECK(screen.px(10 + c1.x + static_cast<int32_t>(c1.width) / 2, 10 + p.y + static_cast<int32_t>(p.height) - 5) != 0xFF112233);   // drawn lighter
    click(w, 10 + 2, 10 + 2);
    CHECK(b.selected_index() == -1);
    press(w, key::kRight);
    CHECK(b.selected_index() == 0);
    press(w, key::kLeft);
    CHECK(b.selected_index() == 3);
    b.set_selected_index(9);
    CHECK(b.selected_index() == -1);

    // Huge values do not overflow.
    b.set_bar_value(0, UINT64_MAX);
    b.set_bar_value(1, UINT64_MAX / 2);
    CHECK(b.scale_top() == UINT64_MAX && b.bar_height(UINT64_MAX) == p.height && near(static_cast<int32_t>(b.bar_height(UINT64_MAX / 2)), static_cast<int32_t>(p.height / 2), 1));
    w.render();

    b.clear_bars();
    CHECK(b.bar_count() == 0 && b.selected_index() == -1);
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_heat_map() {
  TestAllocator a;
  const Theme theme;
  {
    Screen screen(a, 300, 200);
    Window w(a, screen.renderer, theme, {300, 200});
    HeatMap h(a);
    h.set_bounds({10, 10, 260, 160});
    w.add(&h);
    CHECK(h.row_count() == 0 && h.column_count() == 0 && h.grid_rect().width == 0 && h.value(0, 0) == 0);
    int32_t r = 0, c = 0;
    CHECK(!h.cell_at(50, 50, r, c));
    CHECK(!h.set_dimensions(-1, 3) && !h.set_dimensions(3, HeatMap::kMaxDimension + 1) && !h.set_dimensions(1024, 1024) && h.row_count() == 0);
    CHECK(h.set_dimensions(3, 4) && h.row_count() == 3 && h.column_count() == 4);
    h.set_row_label(0, "Mon");
    h.set_row_label(2, "Wed");
    h.set_column_label(3, "Q4");
    h.set_row_label(9, "x");   // ignored
    CHECK(h.row_label(0) == "Mon" && h.row_label(1) == "" && h.row_label(2) == "Wed" && h.column_label(3) == "Q4" && h.row_label(9) == "");
    h.set_value(0, 0, 10);
    h.set_value(1, 1, 55);
    h.set_value(2, 3, 100);
    h.set_value(5, 5, 7);      // ignored
    CHECK(h.value(2, 3) == 100 && h.value(1, 1) == 55 && h.value(5, 5) == 0 && h.value(-1, 0) == 0);

    // The ramp runs from the smallest to the largest value; blue - teal - yellow - red.
    CHECK(h.range_low() == 0 && h.range_high() == 100);   // the zero cells count
    CHECK(h.position(0) == 0 && h.position(100) == 1000 && h.position(50) == 500 && h.position(1000) == 1000);
    CHECK(HeatMap::ramp_color(0) == 0xFF2C4DA8 && HeatMap::ramp_color(1000) == 0xFFD7301F && HeatMap::ramp_color(5000) == 0xFFD7301F);
    CHECK(HeatMap::ramp_color(333) != HeatMap::ramp_color(334) && HeatMap::ramp_color(500) != HeatMap::ramp_color(0));
    h.set_range(20, 60);
    CHECK(h.fixed_range() && h.position(10) == 0 && h.position(40) == 500 && h.position(99) == 1000);
    h.set_range(60, 20);   // swapped
    CHECK(h.range_low() == 20 && h.range_high() == 60);
    h.clear_range();
    CHECK(!h.fixed_range() && h.range_high() == 100);

    // The cells tile the grid, each pixel is in exactly one of them.
    const Rect2D g = h.grid_rect();
    CHECK(g.width > 0 && g.height > 0);
    int32_t area = 0;
    for (int32_t i = 0; i < 3; ++i)
      for (int32_t j = 0; j < 4; ++j) area += static_cast<int32_t>(h.cell_rect(i, j).width * h.cell_rect(i, j).height);
    CHECK(area == static_cast<int32_t>(g.width * g.height));
    for (int32_t i = 0; i < 3; ++i) {
      for (int32_t j = 0; j < 4; ++j) {
        const Rect2D cell = h.cell_rect(i, j);
        CHECK(h.cell_at(cell.x, cell.y, r, c) && r == i && c == j);
        CHECK(h.cell_at(cell.x + static_cast<int32_t>(cell.width) - 1, cell.y + static_cast<int32_t>(cell.height) - 1, r, c) && r == i && c == j);
      }
    }
    CHECK(!h.cell_at(g.x - 1, g.y, r, c) && !h.cell_at(g.x, g.y - 1, r, c) && h.cell_rect(3, 0).width == 0);

    w.render();
    const Rect2D hot = h.cell_rect(2, 3), cold = h.cell_rect(1, 0);
    CHECK(screen.px(10 + hot.x + static_cast<int32_t>(hot.width) / 2, 10 + hot.y + static_cast<int32_t>(hot.height) / 2) == HeatMap::ramp_color(1000));
    CHECK(screen.px(10 + cold.x + static_cast<int32_t>(cold.width) / 2, 10 + cold.y + static_cast<int32_t>(cold.height) / 2) == HeatMap::ramp_color(0));

    // Selection by click and by key; the handler fires only on a change.
    click(w, 10 + hot.x + 3, 10 + hot.y + 3);
    CHECK(h.selected_row() == 2 && h.selected_column() == 3);
    press(w, key::kRight);   // at the edge: stays
    press(w, key::kDown);
    CHECK(h.selected_row() == 2 && h.selected_column() == 3);
    press(w, key::kLeft);
    press(w, key::kUp);
    CHECK(h.selected_row() == 1 && h.selected_column() == 2);
    click(w, 10 + 2, 10 + 2);
    CHECK(h.selected_row() == -1 && h.selected_column() == -1);
    press(w, key::kDown);
    CHECK(h.selected_row() == 0 && h.selected_column() == 0);
    h.select_cell(9, 9);
    CHECK(h.selected_row() == -1);
    h.select_cell(1, 1);
    w.render();
    CHECK(screen.px(10 + h.cell_rect(1, 1).x, 10 + h.cell_rect(1, 1).y) == theme.selection);   // outlined

    // Values printed in the cells.
    h.set_show_values(true);
    CHECK(h.show_values());
    w.render();

    // Too small for a pixel per cell: nothing is drawn, nothing crashes.
    h.set_bounds({10, 10, 12, 12});
    CHECK(h.grid_rect().width == 0 && !h.cell_at(5, 5, r, c));
    w.render();

    // Huge values do not overflow.
    h.set_bounds({10, 10, 260, 160});
    h.set_value(0, 0, UINT64_MAX);
    h.set_value(1, 1, UINT64_MAX / 2);
    CHECK(h.position(UINT64_MAX) == 1000 && near(static_cast<int32_t>(h.position(UINT64_MAX / 2)), 500, 1));
    w.render();

    CHECK(h.set_dimensions(0, 0) && h.row_count() == 0 && h.selected_row() == -1);
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_word_cloud() {
  TestAllocator a;
  const Theme theme;
  {
    Screen screen(a, 300, 200);
    Window w(a, screen.renderer, theme, {300, 200});
    WordCloud wc(a);
    wc.set_bounds({10, 10, 260, 160});
    w.add(&wc);
    CHECK(wc.word_count() == 0 && wc.word_at(50, 50) == -1 && wc.word_rect(0).width == 0 && wc.word_scale(0) == 1);
    CHECK(wc.add_word("big", 100) == 0 && wc.add_word("mid", 50, 0xFF112233) == 1 && wc.add_word("small", 1) == 2 && wc.add_word("tiny", 1) == 3);
    CHECK(wc.word_color(1) == 0xFF112233 && wc.word_color(0) != 0 && wc.word_text(2) == "small" && wc.word_weight(0) == 100);

    // Sizes follow the weights between 1x and 4x.
    CHECK(wc.word_scale(0) == WordCloud::kMaxScale && wc.word_scale(1) == 2 && wc.word_scale(2) == 1 && wc.word_scale(3) == 1);
    wc.set_word_weight(1, 100);
    CHECK(wc.word_scale(1) == WordCloud::kMaxScale);
    wc.set_word_weight(1, 50);
    wc.set_word_weight(9, 5);   // ignored
    {
      WordCloud same(a);
      same.add_word("a", 7);
      same.add_word("b", 7);
      CHECK(same.word_scale(0) == 2 && same.word_scale(1) == 2);
    }

    // Each word has its own place inside the control, sized by its text and scale; no two overlap.
    for (int32_t i = 0; i < 4; ++i) {
      const Rect2D r = wc.word_rect(i);
      CHECK(r.width == static_cast<uint32_t>(8 * wc.word_scale(i)) * wc.word_text(i).size() && r.height == static_cast<uint32_t>(16 * wc.word_scale(i)));
      CHECK(r.x >= 0 && r.y >= 0 && r.x + static_cast<int32_t>(r.width) <= 260 && r.y + static_cast<int32_t>(r.height) <= 160);
      for (int32_t j = i + 1; j < 4; ++j) {
        const Rect2D o = wc.word_rect(j);
        const bool apart = o.x >= r.x + static_cast<int32_t>(r.width) || r.x >= o.x + static_cast<int32_t>(o.width) ||
                           o.y >= r.y + static_cast<int32_t>(r.height) || r.y >= o.y + static_cast<int32_t>(o.height);
        CHECK(apart);
      }
      CHECK(wc.word_at(r.x, r.y) == i && wc.word_at(r.x + static_cast<int32_t>(r.width) - 1, r.y + static_cast<int32_t>(r.height) - 1) == i);
    }
    CHECK(wc.word_at(0, 0) == -1);

    // A narrow control wraps the words onto more lines; the layout follows the width.
    const int32_t one_line_y = wc.word_rect(3).y;
    wc.set_bounds({10, 10, 120, 160});
    CHECK(wc.word_rect(3).y > one_line_y || wc.word_rect(0).y != wc.word_rect(3).y);
    CHECK(wc.word_rect(3).y > wc.word_rect(0).y);
    wc.set_bounds({10, 10, 260, 160});
    CHECK(wc.word_rect(3).y == one_line_y);

    w.render();
    // Selection by click and by key; drawn with the selection colour behind the word.
    const Rect2D r1 = wc.word_rect(1);
    click(w, 10 + r1.x + 2, 10 + r1.y + 2);
    CHECK(wc.selected_index() == 1);
    w.render();
    CHECK(screen.px(10 + r1.x - 1, 10 + r1.y + 1) == theme.selection);
    click(w, 10 + 1, 10 + 1);
    CHECK(wc.selected_index() == -1);
    press(w, key::kRight);
    CHECK(wc.selected_index() == 0);
    press(w, key::kLeft);
    CHECK(wc.selected_index() == 3);
    wc.set_selected_index(9);
    CHECK(wc.selected_index() == -1);

    // Words wider than the control, huge weights, and a lot of words: clipped, no overflow.
    wc.add_word("a-word-far-too-long-to-fit-in-a-control-two-hundred-sixty-pixels-wide", UINT64_MAX);
    wc.set_word_weight(0, UINT64_MAX / 2);
    CHECK(wc.word_scale(4) == WordCloud::kMaxScale && wc.word_scale(0) >= 1 && wc.word_scale(0) <= WordCloud::kMaxScale);
    for (int32_t i = 0; i < 200; ++i) wc.add_word("w", static_cast<uint64_t>(i));
    w.render();

    wc.clear_words();
    CHECK(wc.word_count() == 0 && wc.selected_index() == -1 && wc.word_at(30, 30) == -1);
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_tooltip() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 200);
    Window w(a, screen.renderer, theme, {300, 200});
    Label under(a);
    under.set_bounds({0, 0, 300, 200});
    w.add(&under);
    Tooltip tip(a);
    w.add(&tip);   // after the controls it floats over
    CHECK(!tip.shown() && tip.line_count() == 0);
    tip.show_at(50, 50);   // no text: stays hidden
    CHECK(!tip.shown());

    CHECK(tip.set_text("Save\nCtrl+S"));
    CHECK(tip.line_count() == 2 && tip.text() == "Save\nCtrl+S");
    const Size size = tip.box_size();
    CHECK(size.width == 6 * 8 + 2 * Tooltip::kPadX + 2 && size.height == 2 * 16 + 2 * Tooltip::kPadY + 2);

    // Below and to the right of the pointer, clear of it.
    tip.show_at(50, 50);
    CHECK(tip.shown() && tip.bounds().x > 50 && tip.bounds().y > 50);
    CHECK(tip.bounds().width == static_cast<uint32_t>(size.width) && tip.bounds().height == static_cast<uint32_t>(size.height));
    w.render();
    const Rect2D b = tip.bounds();
    CHECK(screen.px(b.x + b.width - 3, b.y + 3) == Tooltip::kBackground);   // inside the box
    CHECK(screen.px(b.x, b.y) == theme.dark);                               // its border

    // Near the right and bottom edges it flips to the other side of the pointer.
    tip.show_at(295, 195);
    CHECK(tip.bounds().x + static_cast<int32_t>(tip.bounds().width) <= 295 && tip.bounds().y + static_cast<int32_t>(tip.bounds().height) <= 195);
    tip.show_at(0, 0);
    CHECK(tip.bounds().x >= 0 && tip.bounds().y >= 0);

    // Under a control, or above it when there is no room below; always inside the window.
    tip.show_below({20, 30, 60, 20});
    CHECK(tip.bounds().x == 20 && tip.bounds().y == 52);
    tip.show_below({20, 180, 60, 20});
    CHECK(tip.bounds().y + static_cast<int32_t>(tip.bounds().height) <= 180);
    tip.show_below({290, 30, 10, 20});
    CHECK(tip.bounds().x + static_cast<int32_t>(tip.bounds().width) <= 300);

    // The pointer passes through it: a click on the tooltip reaches the control below.
    tip.show_at(100, 100);
    const Rect2D t = tip.bounds();
    CHECK(w.hit_test(t.x + 5, t.y + 5) == &under);

    tip.hide();
    CHECK(!tip.shown());
    w.render();
    CHECK(tip.set_text(""));
    tip.show_at(10, 10);
    CHECK(!tip.shown());
  }
  CHECK(a.live == 0);
}

static int g_toast_clicks = 0;
static void on_toast_click(void*, Control&) { ++g_toast_clicks; }

static void test_toast_center() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 600, 400);
    Window w(a, screen.renderer, theme, {600, 400});
    Label under(a);
    under.set_bounds({0, 0, 600, 400});
    w.add(&under);
    ToastCenter toasts(a);
    w.add(&toasts);
    w.layout();
    CHECK(toasts.count() == 0 && toasts.bounds().width == 0 && toasts.post("", "") == 0);

    // One toast sits in the bottom right corner, a margin from the edges.
    const uint32_t first = toasts.post("Copy finished", "3 files copied to D:", NoticeKind::Information, 1000);
    CHECK(first != 0 && toasts.count() == 1 && toasts.showing(first));
    CHECK(toasts.title_of(first) == "Copy finished" && toasts.message_of(first) == "3 files copied to D:");
    w.layout();
    Rect2D b = toasts.toast_bounds(0);
    CHECK(b.x + static_cast<int32_t>(b.width) == 600 - ToastCenter::kMargin && b.y + static_cast<int32_t>(b.height) == 400 - ToastCenter::kMargin);
    CHECK(b.height == 2 * 16 + 2 * ToastCenter::kPad + 2);
    w.render();
    CHECK(screen.px(b.x + b.width - 4, b.y + b.height - 4) == ToastCenter::kBackground);
    CHECK(screen.px(b.x + 2, b.y + 4) == ToastCenter::kInfoColor);

    // A long message wraps and ends in "..." past three lines; a long title is cut.
    const uint32_t second = toasts.post("A title that is a good deal longer than the toast can show", "The quick brown fox jumps over the lazy dog and keeps running through the whole field until the sun goes down behind the hills far away", NoticeKind::Error);
    CHECK(second != first && toasts.title_of(second).size() == ToastCenter::kTitleChars);
    CHECK(toasts.message_of(second).size() <= 3 * 41 && toasts.message_of(second).substr(toasts.message_of(second).size() - 3) == "...");
    CHECK(toasts.kind_of(second) == NoticeKind::Error);
    w.layout();
    CHECK(toasts.toast_bounds(0).y < toasts.toast_bounds(1).y);   // the newest is the lowest
    CHECK(toasts.toast_bounds(1).y + static_cast<int32_t>(toasts.toast_bounds(1).height) == 400 - ToastCenter::kMargin);

    // The pointer reaches what is under the center, except where a toast is.
    CHECK(w.hit_test(10, 10) == &under);
    const Rect2D top = toasts.toast_bounds(0);
    CHECK(w.hit_test(top.x + 5, top.y + 5) == &toasts);

    // Time starts at the first tick (10 s); the first toast (1 s) goes at 11 s, the second (5 s) at 15 s.
    CHECK(toasts.tick(10000000) == 0 && toasts.count() == 2);
    CHECK(toasts.tick(10500000) == 0);
    CHECK(toasts.tick(11000000) == 1 && !toasts.showing(first) && toasts.showing(second));
    CHECK(toasts.tick(14999999) == 0 && toasts.tick(15000000) == 1 && toasts.count() == 0);
    w.layout();
    CHECK(toasts.bounds().width == 0 && w.hit_test(top.x + 5, top.y + 5) == &under);

    // A toast with duration 0 stays; clicking it fires its handler and removes it. The [x] removes it without the handler.
    g_toast_clicks = 0;
    const uint32_t sticky = toasts.post("Printer", "Out of paper", NoticeKind::Warning, 0, Handler::immediate(&on_toast_click, nullptr));
    toasts.tick(1);
    toasts.tick(1000000000000ull);
    CHECK(toasts.showing(sticky));
    w.layout();
    b = toasts.toast_bounds(0);
    w.mouse_button(b.x + 40, b.y + b.height - 8, MouseButton::Left, true);
    w.mouse_button(b.x + 40, b.y + b.height - 8, MouseButton::Left, false);
    CHECK(g_toast_clicks == 1 && toasts.count() == 0);
    const uint32_t other = toasts.post("Printer", "Out of paper", NoticeKind::Warning, 0, Handler::immediate(&on_toast_click, nullptr));
    w.layout();
    b = toasts.toast_bounds(0);
    w.mouse_button(b.x + b.width - 10, b.y + 6, MouseButton::Left, true);
    w.mouse_button(b.x + b.width - 10, b.y + 6, MouseButton::Left, false);
    CHECK(g_toast_clicks == 1 && !toasts.showing(other) && toasts.count() == 0);

    // At most kMaxToasts: the oldest makes room. dismiss() and clear().
    uint32_t ids[ToastCenter::kMaxToasts + 2];
    for (uint32_t& id : ids) id = toasts.post("t", "m");
    CHECK(toasts.count() == ToastCenter::kMaxToasts && !toasts.showing(ids[0]) && !toasts.showing(ids[1]) && toasts.showing(ids[2]));
    CHECK(toasts.dismiss(ids[3]) && !toasts.dismiss(ids[3]) && toasts.count() == ToastCenter::kMaxToasts - 1);
    toasts.clear();
    CHECK(toasts.count() == 0);

    // An area (a monitor above its taskbar) moves the corner.
    toasts.set_area({0, 0, 400, 200});
    toasts.post("Hi", "there");
    w.layout();
    b = toasts.toast_bounds(0);
    CHECK(b.x + static_cast<int32_t>(b.width) == 400 - ToastCenter::kMargin && b.y + static_cast<int32_t>(b.height) == 200 - ToastCenter::kMargin);
    toasts.clear_area();
    w.layout();
    CHECK(toasts.toast_bounds(0).x + static_cast<int32_t>(toasts.toast_bounds(0).width) == 600 - ToastCenter::kMargin);
  }
  CHECK(a.live == 0);
}

static void test_scroll_panel() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 200);
    Window w(a, screen.renderer, theme, {300, 200});
    ScrollPanel sp(a);
    sp.set_bounds({10, 10, 200, 100});
    w.add(&sp);
    // Ten rows of 22 pixels do not fit in 100: a scroll bar shows and the rows are narrower by it.
    Label rows[10] = {Label(a), Label(a), Label(a), Label(a), Label(a), Label(a), Label(a), Label(a), Label(a), Label(a)};
    for (Label& r : rows) { r.set_size(0, 22); sp.add(&r); }
    w.layout();
    CHECK(sp.content_height() == 220 && sp.scroll_bar_visible() && sp.scroll_offset() == 0);
    CHECK(rows[0].bounds().y == 0 && rows[1].bounds().y == 22 && rows[9].bounds().y == 198 && rows[0].bounds().width == 200 - ScrollBar::kThickness);
    CHECK(sp.scroll_bar().total() == 220 && sp.scroll_bar().page() == 100);
    // Scrolling moves the rows; it stops at the end.
    sp.scroll_to(50);
    CHECK(sp.scroll_offset() == 50 && rows[0].bounds().y == -50 && rows[3].bounds().y == 16);
    sp.scroll_to(1000);
    CHECK(sp.scroll_offset() == 120 && rows[9].bounds().y + 22 == 100);
    sp.scroll_to(0);
    // The wheel scrolls from anywhere over the panel, over a label too (which takes no wheel itself).
    w.mouse_wheel(30, 20, -1);
    CHECK(sp.scroll_offset() == 66);   // three steps of 22
    w.mouse_wheel(30, 20, 1);
    CHECK(sp.scroll_offset() == 0);
    // Rows that are hidden take no room; when all fit the bar goes away.
    for (int i = 4; i < 10; ++i) rows[i].set_visible(false);
    w.layout();
    CHECK(sp.content_height() == 88 && !sp.scroll_bar_visible() && rows[0].bounds().width == 200);
  }
  CHECK(a.live == 0);
}

static void test_checkbox() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 100);
    Window w(a, screen.renderer, theme, {300, 100});
    CheckBox c(a);
    c.set_text("Enable echo");
    c.set_bounds({10, 10, 150, 20});
    w.add(&c);
    Counter changes;
    c.on_changed(changes.handler());
    CHECK(!c.checked());
    CHECK(c.preferred_size().width == 13 + 6 + 11 * 8 + 4);

    click_center(w, c);
    CHECK(c.checked() && changes.count == 1 && w.focus() == &c);
    click_center(w, c);
    CHECK(!c.checked() && changes.count == 2);

    // Pressing and releasing elsewhere is not a click.
    w.mouse_move(20, 20);
    w.mouse_button(20, 20, MouseButton::Left, true);
    w.mouse_move(250, 80);
    w.mouse_button(250, 80, MouseButton::Left, false);
    CHECK(!c.checked() && changes.count == 2);

    press(w, key::kSpace);
    CHECK(c.checked() && changes.count == 3);
    c.set_checked(true);  // no change, no event
    CHECK(changes.count == 3);

    c.set_enabled(false);
    click_center(w, c);
    press(w, key::kSpace);
    CHECK(c.checked() && changes.count == 3);
    c.set_enabled(true);

    // The box is drawn white, with a tick when checked (dark pixels inside), and grey when disabled.
    w.render();
    const int32_t box_top = 10 + (20 - 13) / 2;
    CHECK(screen.px(10 + 3, box_top + 3) == theme.field);
    CHECK(screen.px(10 + 5, box_top + 8) == theme.text);   // the tick's low point
    c.set_checked(false);
    w.render();
    CHECK(screen.px(10 + 5, box_top + 8) == theme.field);
  }
  CHECK(a.live == 0);
}

static void test_radio() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 150);
    Window w(a, screen.renderer, theme, {300, 150});
    RadioGroup group;
    RadioButton r1(a), r2(a), r3(a);
    RadioButton* all[3] = {&r1, &r2, &r3};
    const char* names[3] = {"Low", "Medium", "High"};
    for (int i = 0; i < 3; ++i) {
      all[i]->set_text(names[i]);
      all[i]->set_bounds({10, 10 + 24 * i, 120, 20});
      all[i]->set_group(&group);
      w.add(all[i]);
    }
    Counter c1, c2, c3;
    r1.on_changed(c1.handler()); r2.on_changed(c2.handler()); r3.on_changed(c3.handler());
    CHECK(group.count() == 3 && group.at(1) == &r2 && group.selected() == nullptr && group.selected_index() == -1);

    click_center(w, r2);
    CHECK(r2.checked() && group.selected() == &r2 && group.selected_index() == 1 && c2.count == 1);
    click_center(w, r2);   // clicking the checked one changes nothing
    CHECK(r2.checked() && c2.count == 1);
    click_center(w, r3);
    CHECK(r3.checked() && !r2.checked() && c3.count == 1 && c2.count == 2 && c1.count == 0);

    group.select(0);
    CHECK(r1.checked() && !r3.checked() && group.selected_index() == 0);
    group.select(9);  // no such button
    CHECK(group.selected_index() == 0);

    // Arrow keys move the check and the focus, wrapping round and skipping buttons that cannot be used.
    w.set_focus(&r1);
    press(w, key::kDown);
    CHECK(r2.checked() && w.focus() == &r2 && !r1.checked());
    press(w, key::kRight);
    CHECK(r3.checked() && w.focus() == &r3);
    press(w, key::kDown);   // wraps to the first
    CHECK(r1.checked() && w.focus() == &r1);
    press(w, key::kUp);     // and back to the last
    CHECK(r3.checked() && w.focus() == &r3);
    r2.set_enabled(false);
    press(w, key::kUp);
    CHECK(r1.checked() && w.focus() == &r1);
    r2.set_enabled(true);
    r1.set_checked(false);
    CHECK(group.selected() == nullptr);
    press(w, key::kSpace);
    CHECK(r1.checked());

    // Drawn as a disc: lit centre dot only on the checked one.
    w.render();
    CHECK(screen.px(10 + 6, 10 + 10) == theme.text);
    CHECK(screen.px(10 + 6, 10 + 24 + 10) == theme.field);

    // A button that goes away leaves the group; two groups are independent.
    {
      RadioButton temp(a);
      temp.set_group(&group);
      CHECK(group.count() == 4);
    }
    CHECK(group.count() == 3);
    RadioGroup other;
    RadioButton lone(a);
    lone.set_group(&other);
    lone.set_checked(true);
    r2.set_checked(true);
    CHECK(lone.checked() && r2.checked());
    r3.set_group(&other);   // moving a button
    CHECK(group.count() == 2 && other.count() == 2);
    CHECK(other.selected() == &lone);
  }
  CHECK(a.live == 0);
}

static void test_toggle() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 100);
    Window w(a, screen.renderer, theme, {300, 100});
    Toggle t(a);
    t.set_text("Mute");
    t.set_bounds({10, 10, 120, 24});
    w.add(&t);
    Counter changes;
    t.on_changed(changes.handler());
    CHECK(!t.on());
    w.render();
    const int32_t top = 10 + (24 - Toggle::kTrackHeight) / 2;
    CHECK(screen.px(10 + 5, top + 9) == theme.shadow || screen.px(10 + 5, top + 9) == theme.face);  // knob on the left, off
    CHECK(screen.px(10 + 30, top + 9) == theme.field);

    click_center(w, t);
    CHECK(t.on() && changes.count == 1 && w.focus() == &t);
    w.render();
    CHECK(screen.px(10 + 5, top + 9) == theme.selection);   // the track shows through where the knob left
    press(w, key::kSpace);
    CHECK(!t.on() && changes.count == 2);
    t.set_on(false);
    CHECK(changes.count == 2);
    t.set_enabled(false);
    click_center(w, t);
    CHECK(!t.on() && changes.count == 2);
  }
  CHECK(a.live == 0);
}

static void test_color_picker() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 400, 300);
    Window w(a, screen.renderer, theme, {400, 300});
    ColorPicker p(a);
    p.set_bounds({10, 10, 112, 24});
    w.add(&p);
    Counter changes;
    p.on_color_changed(changes.handler());
    p.set_color(0xFFFF0000);
    CHECK(p.color() == 0xFFFF0000 && p.hsv().hue == 0 && p.hsv().saturation == 255 && changes.count == 0);  // it was red already
    p.set_color(0x80336699);   // the alpha is ignored
    CHECK(p.color() == 0xFF336699 && changes.count == 1);
    p.set_color(0xFFFF0000);
    changes.count = 0;

    w.render();
    CHECK(screen.px(10 + 8, 10 + 12) == 0xFFFF0000);   // the swatch

    click_center(w, p);
    CHECK(p.open() && w.popup() != nullptr);
    const Rect2D pop = w.popup()->bounds();
    CHECK(pop.x == 10 && pop.y == 34 && pop.width == ColorPickerPopup::kWidth && pop.height == ColorPickerPopup::kHeight);
    w.render();
    // The square at hue 0: top right is pure red, top left white, bottom anything is black.
    CHECK(screen.px(pop.x + 6 + 127, pop.y + 6 + 2) == 0xFFFF0000);
    CHECK(screen.px(pop.x + 6 + 2, pop.y + 6 + 2) == 0xFFFFFFFF);
    CHECK(screen.px(pop.x + 6 + 60, pop.y + 6 + 127) == 0xFF000000);

    // Click in the square: saturation across, brightness down.
    click(w, pop.x + 6, pop.y + 6 + 127);                  // bottom left: black
    CHECK(p.color() == 0xFF000000 && changes.count == 1 && p.open());
    click(w, pop.x + 6, pop.y + 6);                        // top left: white
    CHECK(p.color() == 0xFFFFFFFF && changes.count == 2);
    click(w, pop.x + 6 + 127, pop.y + 6);                  // top right: the full colour of the hue
    CHECK(p.color() == 0xFFFF0000 && changes.count == 3);

    // Drag in the hue strip, even past its ends.
    const int32_t strip_x = pop.x + 6 + 128 + 8 + 8;
    w.mouse_move(strip_x, pop.y + 6);
    w.mouse_button(strip_x, pop.y + 6, MouseButton::Left, true);
    w.mouse_move(strip_x + 30, pop.y + 6 + 64);            // halfway down (x does not matter while dragging)
    CHECK(near(p.hsv().hue, 180, 3) && p.hsv().saturation == 255 && p.hsv().value == 255);
    CHECK(near(red_of(p.color()), 0, 0) && blue_of(p.color()) > 240);
    w.mouse_move(strip_x, pop.y + 400);
    CHECK(p.hsv().hue == 359);
    w.mouse_button(strip_x, pop.y + 400, MouseButton::Left, false);
    w.mouse_move(strip_x, pop.y + 6);   // released: the move does nothing
    CHECK(p.hsv().hue == 359);

    // Keys in the popup: saturation, brightness, hue; the popup swallows other keys.
    p.set_hsv({100, 100, 100});
    press(w, key::kLeft);
    CHECK(p.hsv().saturation == 92);
    press(w, key::kUp);
    CHECK(p.hsv().value == 108);
    press(w, key::kDown);
    press(w, key::kDown);
    CHECK(p.hsv().value == 92);
    press(w, 0x4B);
    CHECK(p.hsv().hue == 115);
    press(w, 0x4E);
    press(w, 0x4E);
    CHECK(p.hsv().hue == 85);
    press(w, key::kRight);
    for (int i = 0; i < 40; ++i) press(w, key::kRight);   // clamps at 255
    CHECK(p.hsv().saturation == 255);
    press(w, key::kTab);
    CHECK(p.open());
    press(w, key::kEscape);
    CHECK(!p.open() && w.popup() == nullptr);

    // Keyboard on the closed control; clicking outside closes without a change.
    press(w, key::kSpace);
    CHECK(p.open());
    const uint32_t before = p.color();
    click(w, 300, 250);
    CHECK(!p.open() && p.color() == before);
    press(w, key::kDown, hardware::modifier::kLeftAlt);
    CHECK(p.open());
    press(w, key::kEnter);
    CHECK(!p.open());

    // A grey keeps the hue it was picked with.
    p.set_hsv({200, 150, 150});
    p.set_color(0xFF808080);
    CHECK(p.hsv().saturation == 0 && p.hsv().hue == 200);
    p.set_color(0xFF000000);
    CHECK(p.hsv().value == 0 && p.hsv().hue == 200);
  }
  CHECK(a.live == 0);
}

static void test_date_picker() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 400, 300);
    Window w(a, screen.renderer, theme, {400, 300});
    DatePicker d(a);
    d.set_bounds({10, 10, 130, 24});
    w.add(&d);
    Counter changes;
    d.on_date_changed(changes.handler());
    CHECK(d.date() == Date{2000, 1, 1});
    d.set_date({2026, 10, 17});
    CHECK(d.date() == Date{2026, 10, 17} && changes.count == 1);
    d.set_date({2026, 2, 30});   // not a date
    d.set_date({2026, 10, 17});  // unchanged
    CHECK(d.date() == Date{2026, 10, 17} && changes.count == 1);

    // Click: the calendar for October 2026 opens under the field.
    click_center(w, d);
    CHECK(d.open() && w.popup() == &d.popup());
    const Rect2D pop = w.popup()->bounds();
    CHECK(pop.x == 10 && pop.y == 34 && pop.width == DatePickerPopup::kWidth && pop.height == DatePickerPopup::kHeight);
    CHECK(d.popup().month() == Date{2026, 10, 1});
    // 1 October 2026 is a Thursday: the grid starts on Sunday 27 September.
    CHECK(weekday({2026, 10, 1}) == 4);
    const Rect2D c17 = d.popup().cell_of({2026, 10, 17});
    CHECK(c17.x == 6 + 6 * 26 && c17.y == 46 + 2 * 20);
    w.render();
    CHECK(screen.px(pop.x + c17.x + 3, pop.y + c17.y + 3) == theme.selection);   // the chosen day
    const Rect2D c20 = d.popup().cell_of({2026, 10, 20});
    CHECK(screen.px(pop.x + c20.x + 3, pop.y + c20.y + 3) == theme.field);

    // Click a day: chosen, popup closed.
    click(w, pop.x + c20.x + 13, pop.y + c20.y + 10);
    CHECK(!d.open() && d.date() == Date{2026, 10, 20} && changes.count == 2);

    // The month arrows turn the page without closing; a day of the next month can be clicked.
    click_center(w, d);
    const int32_t arrow_y = pop.y + 6 + 11;
    click(w, pop.x + DatePickerPopup::kWidth - 6 - 11, arrow_y);   // next month
    CHECK(d.open() && d.popup().month() == Date{2026, 11, 1});
    click(w, pop.x + DatePickerPopup::kWidth - 6 - 11, arrow_y);
    click(w, pop.x + 6 + 11, arrow_y);                             // previous
    CHECK(d.popup().month() == Date{2026, 11, 1});
    click(w, pop.x + 6 + 11, arrow_y);
    click(w, pop.x + 6 + 11, arrow_y);
    CHECK(d.popup().month() == Date{2026, 9, 1});
    const Rect2D oct1 = d.popup().cell_of({2026, 10, 1});          // the grid shows the first days of the next month too
    click(w, pop.x + oct1.x + 5, pop.y + oct1.y + 5);
    CHECK(!d.open() && d.date() == Date{2026, 10, 1} && changes.count == 3);

    // Keyboard in the calendar: the cursor moves, Enter chooses; the view follows the cursor.
    d.set_date({2026, 1, 31});
    changes.count = 0;
    press(w, key::kSpace);   // the field has the focus from the clicks
    CHECK(d.open() && d.popup().cursor() == Date{2026, 1, 31});
    press(w, key::kRight);
    CHECK(d.popup().cursor() == Date{2026, 2, 1} && d.popup().month() == Date{2026, 2, 1});
    press(w, key::kDown);
    CHECK(d.popup().cursor() == Date{2026, 2, 8});
    press(w, key::kUp);
    press(w, key::kUp);
    CHECK(d.popup().cursor() == Date{2026, 1, 25});
    press(w, 0x4E);   // Page Down: next month, same day
    CHECK(d.popup().cursor() == Date{2026, 2, 25});
    press(w, key::kTab);
    CHECK(d.open());
    press(w, key::kEnter);
    CHECK(!d.open() && d.date() == Date{2026, 2, 25} && changes.count == 1);

    // Escape and clicking outside leave the date alone.
    press(w, key::kSpace);
    press(w, key::kRight);
    press(w, key::kEscape);
    CHECK(!d.open() && d.date() == Date{2026, 2, 25});
    press(w, key::kSpace);
    click(w, 350, 250);
    CHECK(!d.open() && d.date() == Date{2026, 2, 25});

    // The closed control: arrows change the date; months clamp the day.
    d.set_date({2026, 1, 31});
    changes.count = 0;
    press(w, key::kRight);
    CHECK(d.date() == Date{2026, 2, 1});
    press(w, key::kLeft);
    press(w, key::kUp);
    CHECK(d.date() == Date{2026, 1, 24});
    press(w, key::kDown);
    press(w, key::kDown);
    CHECK(d.date() == Date{2026, 2, 7});
    d.set_date({2026, 1, 31});
    press(w, 0x4E);
    CHECK(d.date() == Date{2026, 2, 28});
    press(w, 0x4B);
    CHECK(d.date() == Date{2026, 1, 28});
    d.set_date({9999, 12, 31});
    press(w, key::kRight);   // the last day there is
    CHECK(d.date() == Date{9999, 12, 31});
    press(w, key::kDown, hardware::modifier::kLeftAlt);
    CHECK(d.open());
    w.close_popup();

    // The field shows YYYY-MM-DD.
    d.set_date({2026, 3, 9});
    w.render();
    CHECK(d.date() == Date{2026, 3, 9});
  }
  CHECK(a.live == 0);
}

int main() {
  test_helpers();
  test_slider();
  test_knob();
  test_level_meter();
  test_pie_chart();
  test_bar_chart();
  test_heat_map();
  test_word_cloud();
  test_tooltip();
  test_toast_center();
  test_scroll_panel();
  test_checkbox();
  test_radio();
  test_toggle();
  test_color_picker();
  test_date_picker();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gui controls: all checks passed\n");
  return 0;
}
