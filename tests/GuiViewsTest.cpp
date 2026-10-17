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

// Host-side tests for the larger controls: ScrollBar, DataGrid (with its models), ImageView and the audio
// player control (over a fake audio device and the real mixer and player).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Audio/AudioPlayer.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Gui/Controls/AudioPlayerControl.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/ImageView.hpp"
#include "Gui/Controls/ScrollBar.hpp"
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
static void type_char(Window& w, char c) {
  KeyArgs k;
  k.character = c;
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

static void test_scroll_bar() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 300);
    Window w(a, screen.renderer, theme, {300, 300});
    ScrollBar s(a, Orientation::Vertical);
    s.set_bounds({10, 10, 16, 116});   // a 84-pixel track between two 16-pixel arrows
    w.add(&s);
    Counter changes;
    s.on_value_changed(changes.handler());

    // Everything fits: no thumb, nothing moves.
    s.set_metrics(10, 10);
    CHECK(s.thumb_rect().height == 0 && s.maximum_value() == 0);
    click(w, 18, 10 + 116 - 8);
    CHECK(s.value() == 0 && changes.count == 0);

    // 100 units, 25 in view: a quarter of the track.
    s.set_metrics(100, 25);
    CHECK(s.maximum_value() == 75 && s.thumb_rect().y == 16 && s.thumb_rect().height == 21);
    click(w, 18, 10 + 116 - 8);   // the lower arrow
    CHECK(s.value() == 1 && changes.count == 1);
    click(w, 18, 10 + 8);         // the upper arrow
    CHECK(s.value() == 0);
    click(w, 18, 10 + 8);         // at the start already
    CHECK(s.value() == 0);
    s.set_step(5);
    click(w, 18, 10 + 116 - 8);
    CHECK(s.value() == 5);

    // The track: a page at a time, towards the click.
    click(w, 18, 10 + 16 + 70);   // below the thumb
    CHECK(s.value() == 30);
    click(w, 18, 10 + 16 + 70);
    CHECK(s.value() == 55);
    click(w, 18, 10 + 16 + 70);
    CHECK(s.value() == 75);       // clamped at the end
    CHECK(s.thumb_rect().y + static_cast<int32_t>(s.thumb_rect().height) == 16 + 84);
    click(w, 18, 10 + 16 + 2);    // above the thumb
    CHECK(s.value() == 50);

    // Dragging the thumb keeps the grab point.
    s.set_value(0);
    const int32_t thumb_y = 10 + s.thumb_rect().y;
    w.mouse_move(18, thumb_y + 5);
    w.mouse_button(18, thumb_y + 5, MouseButton::Left, true);
    w.mouse_move(18, thumb_y + 5 + 31);   // half the room (63) down
    CHECK(near(s.value(), 37, 2));
    w.mouse_move(18, 10 + 400);
    CHECK(s.value() == 75);
    w.mouse_move(18, 10 - 400);
    CHECK(s.value() == 0);
    w.mouse_button(18, 10 - 400, MouseButton::Left, false);
    w.mouse_move(18, 10 + 100);
    CHECK(s.value() == 0);

    // The wheel: three steps, wheel up goes back.
    s.set_step(2);
    w.mouse_wheel(18, 50, -1);
    CHECK(s.value() == 6);
    w.mouse_wheel(18, 50, 1);
    w.mouse_wheel(18, 50, 1);
    CHECK(s.value() == 0);

    // Shrinking the content keeps the value in range; the handler only fires for real changes.
    s.set_value(60);
    changes.count = 0;
    s.set_metrics(70, 25);
    CHECK(s.value() == 45 && changes.count == 1);
    s.set_value(45);
    CHECK(changes.count == 1);

    // Horizontal.
    ScrollBar h(a, Orientation::Horizontal);
    h.set_bounds({10, 200, 116, 16});
    w.add(&h);
    h.set_metrics(200, 50);
    CHECK(h.thumb_rect().x == 16 && h.thumb_rect().width == 21 && h.thumb_rect().height == 16);
    click(w, 10 + 116 - 8, 208);
    CHECK(h.value() == 1);
    w.render();
  }
  CHECK(a.live == 0);
}

// --- DataGrid ------------------------------------------------------------------

struct NumbersModel final : IGridModel {  // row r: its number, its square, "even"/"odd"
  uint32_t rows;
  explicit NumbersModel(uint32_t n) : rows(n) {}
  uint32_t row_count() const override { return rows; }
  bool cell_text(uint32_t row, uint32_t column, String* out) const override {
    out->assign(StringView());
    if (column == 0) return out->append_uint(row);
    if (column == 1) return out->append_uint(uint64_t{row} * row);
    return out->append(row % 2 ? "odd" : "even");
  }
};

static void add_person(DataGrid& g, const char* name, const char* age, const char* city) {
  const StringView cells[3] = {name, age, city};
  CHECK(g.add_row(cells, 3) >= 0);
}

static void test_data_grid() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 500, 400);
    Window w(a, screen.renderer, theme, {500, 400});
    DataGrid g(a);
    g.set_bounds({10, 10, 300, 200});
    w.add(&g);
    CHECK(g.add_column("Name", 120) == 0 && g.add_column("Age", 60, Align::Right) == 1 && g.add_column("City", 100) == 2);
    CHECK(g.column_count() == 3 && g.column_title(1).equals("Age") && g.column_width(1) == 60);
    CHECK(g.add_column("Tiny", 5) == 3 && g.column_width(3) == DataGrid::kMinColumnWidth);
    // (a fourth column makes the table wider than the grid: 120+60+100+24 = 304 > 296)
    w.layout();
    CHECK(g.horizontal_scroll_bar().visible());
    g.set_column_width(3, 5);
    CHECK(g.row_count() == 0 && g.selected_row() == -1);

    add_person(g, "Zed", "40", "Oslo");
    add_person(g, "amy", "7", "Rome");
    add_person(g, "Bob", "100", "Bern");
    add_person(g, "Cy", "7", "Lima");
    CHECK(g.row_count() == 4 && g.add_column("Late", 50) == -1);   // not once rows exist
    w.layout();
    CHECK(!g.vertical_scroll_bar().visible());
    Counter picked, activated, sorted;
    g.on_selection_changed(picked.handler());
    g.on_row_activated(activated.handler());
    g.on_sort_changed(sorted.handler());

    // Click on the second row.
    const Rect2D body = g.body_rect();
    click(w, 10 + 30, 10 + body.y + DataGrid::kRowHeight + 4);
    CHECK(g.selected_row() == 1 && picked.count == 1 && w.focus() == &g);
    click(w, 10 + 30, 10 + body.y + DataGrid::kRowHeight + 4);   // same row again: no event
    CHECK(picked.count == 1);
    click(w, 10 + 30, 10 + body.y + 100);                        // below the last row: nothing
    CHECK(g.selected_row() == 1);

    // Sort by name: without regard to case, ascending then descending; the selection follows its row.
    click(w, 10 + 30, 10 + 5);
    CHECK(g.sort_column() == 0 && g.sort_ascending() && sorted.count == 1);
    CHECK(g.row_at_position(0) == 1 && g.row_at_position(1) == 2 && g.row_at_position(2) == 3 && g.row_at_position(3) == 0);  // amy Bob Cy Zed
    CHECK(g.selected_row() == 1 && picked.count == 1);
    click(w, 10 + 30, 10 + 5);
    CHECK(!g.sort_ascending() && g.row_at_position(0) == 0 && g.row_at_position(3) == 1 && sorted.count == 2);

    // Sort by age: numbers sort as numbers (7, 7, 40, 100), equal ones keep their order (stable).
    click(w, 10 + 120 + 20, 10 + 5);
    CHECK(g.sort_column() == 1 && g.sort_ascending());
    CHECK(g.row_at_position(0) == 1 && g.row_at_position(1) == 3 && g.row_at_position(2) == 0 && g.row_at_position(3) == 2);
    CHECK(g.sort_by(1, false) && g.row_at_position(0) == 2 && g.row_at_position(1) == 0 && g.row_at_position(2) == 1 && g.row_at_position(3) == 3);
    CHECK(g.sort_by(-1) && g.row_at_position(0) == 0 && g.row_at_position(3) == 3);
    CHECK(!g.sort_by(7));

    // Pressing a header and releasing elsewhere is not a click.
    const int32_t sorted_before = sorted.count;
    w.mouse_move(10 + 30, 10 + 5);
    w.mouse_button(10 + 30, 10 + 5, MouseButton::Left, true);
    w.mouse_move(10 + 30, 10 + 100);
    w.mouse_button(10 + 30, 10 + 100, MouseButton::Left, false);
    CHECK(sorted.count == sorted_before);

    // Keyboard: arrows move the selection along the screen order; Enter activates.
    g.select_row(-1);
    CHECK(g.selected_row() == -1);
    press(w, key::kDown);
    CHECK(g.selected_row() == 0);
    press(w, key::kDown);
    press(w, key::kDown);
    CHECK(g.selected_row() == 2);
    press(w, key::kUp);
    CHECK(g.selected_row() == 1);
    press(w, 0x4D);   // End
    CHECK(g.selected_row() == 3);
    press(w, key::kDown);   // nothing beyond the last
    CHECK(g.selected_row() == 3);
    press(w, 0x4A);   // Home
    CHECK(g.selected_row() == 0);
    press(w, key::kEnter);
    CHECK(activated.count == 1 && activated.last == &g);
    g.select_row(99);   // no such row
    CHECK(g.selected_row() == 0);

    // Changing a cell of the sorted column re-sorts.
    g.sort_by(0, true);
    CHECK(g.set_cell(0, 0, "aaa"));
    CHECK(g.row_at_position(0) == 0);
    CHECK(!g.set_cell(9, 0, "x") && !g.set_cell(0, 9, "x"));

    // Resizing: the border of the first header, dragged right; a minimum width holds.
    const int32_t border_x = 10 + g.column_left(0) + g.column_width(0);
    w.mouse_move(border_x, 10 + 5);
    w.mouse_button(border_x, 10 + 5, MouseButton::Left, true);
    w.mouse_move(border_x + 30, 10 + 5);
    CHECK(g.column_width(0) == 150);
    w.mouse_move(border_x - 500, 10 + 5);
    CHECK(g.column_width(0) == DataGrid::kMinColumnWidth);
    w.mouse_button(border_x - 500, 10 + 5, MouseButton::Left, false);
    CHECK(g.sort_column() == 0);   // resizing is not a click on the header
    g.set_column_width(0, 120);

    g.clear_rows();
    CHECK(g.row_count() == 0 && g.selected_row() == -1);
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_data_grid_scrolling() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 500, 400);
    Window w(a, screen.renderer, theme, {500, 400});
    DataGrid g(a);
    g.set_bounds({10, 10, 300, 200});
    w.add(&g);
    g.add_column("n", 100, Align::Right);
    g.add_column("n squared", 100, Align::Right);
    g.add_column("parity", 200);   // 400 wide in all: more than the 296 shown
    NumbersModel model(1000);
    g.set_model(&model);
    w.layout();
    CHECK(g.row_count() == 1000 && g.vertical_scroll_bar().visible() && g.horizontal_scroll_bar().visible());
    const uint32_t visible = g.visible_row_count();
    CHECK(visible == (200 - 4 - 20 - 16) / 18);   // header and the horizontal bar take their share
    CHECK(g.vertical_scroll_bar().total() == 1000 && g.vertical_scroll_bar().page() == static_cast<int32_t>(visible));

    // The wheel scrolls three rows; the bar follows; and so does the bar's drag.
    w.mouse_wheel(100, 100, -1);
    CHECK(g.first_visible_position() == 3 && g.vertical_scroll_bar().value() == 3);
    w.mouse_wheel(100, 100, 1);
    w.mouse_wheel(100, 100, 1);
    CHECK(g.first_visible_position() == 0);
    g.vertical_scroll_bar().set_value(500);
    CHECK(g.first_visible_position() == 500);
    w.render();
    // The first visible row is number 500: its text is in the top row (white or grey background, black text pixels exist).
    CHECK(g.row_at_position(500) == 500);

    // Selecting scrolls into view.
    w.set_focus(&g);
    g.select_row(0);
    CHECK(g.first_visible_position() == 0);
    g.select_row(999);
    CHECK(g.first_visible_position() == 1000 - visible);
    press(w, key::kUp);
    CHECK(g.selected_row() == 998);
    press(w, 0x4B);   // Page Up
    CHECK(g.selected_row() == 998 - static_cast<int32_t>(visible - 1));
    press(w, 0x4A);
    CHECK(g.selected_row() == 0 && g.first_visible_position() == 0);

    // Sideways: the keys and the bar.
    CHECK(g.horizontal_offset() == 0);
    press(w, key::kRight);
    CHECK(g.horizontal_offset() == 16 && g.horizontal_scroll_bar().value() == 16);
    for (int i = 0; i < 50; ++i) press(w, key::kRight);
    CHECK(g.horizontal_offset() == 400 - static_cast<int32_t>(g.body_rect().width));
    press(w, key::kLeft);
    CHECK(g.horizontal_offset() == 400 - static_cast<int32_t>(g.body_rect().width) - 16);
    g.horizontal_scroll_bar().set_value(0);
    CHECK(g.horizontal_offset() == 0);

    // Sorting a big model (numbers by value, descending) and clicking a row picks the right model row.
    CHECK(g.sort_by(1, false));
    CHECK(g.row_at_position(0) == 999 && g.row_at_position(999) == 0);
    g.vertical_scroll_bar().set_value(0);
    click(w, 10 + 20, 10 + g.body_rect().y + 4);
    CHECK(g.selected_row() == 999);

    // The model changes under the grid.
    model.rows = 5;
    g.model_changed();
    CHECK(g.row_count() == 5 && g.selected_row() == -1 && !g.vertical_scroll_bar().visible() && g.first_visible_position() == 0);
    CHECK(g.row_at_position(0) == 4 && g.row_at_position(4) == 0);

    // Back to the grid's own (empty) table.
    g.set_model(nullptr);
    CHECK(g.row_count() == 0);
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_grid_rendering() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 400, 300);
    Window w(a, screen.renderer, theme, {400, 300});
    DataGrid g(a);
    g.set_bounds({10, 10, 300, 200});
    w.add(&g);
    g.add_column("A", 100);
    g.add_column("B", 100);
    const StringView r0[2] = {"x", "y"}, r1[2] = {"p", "q"};
    g.add_row(r0, 2);
    g.add_row(r1, 2);
    w.layout();
    w.render();
    const Rect2D body = g.body_rect();
    CHECK(screen.px(10 + 150, 10 + 8) == theme.face);                 // header
    CHECK(screen.px(10 + 250, 10 + body.y + 4) == theme.field);       // first row, past the text
    CHECK(screen.px(10 + 250, 10 + body.y + DataGrid::kRowHeight + 4) == 0xFFF2F2F2);  // second row: the alternate tone
    CHECK(screen.px(10 + 250, 10 + body.y + 3 * DataGrid::kRowHeight) == theme.field);  // below the rows
    g.select_row(0);
    w.render();
    CHECK(screen.px(10 + 90, 10 + body.y + 2) == theme.shadow);   // selected, but the grid has no focus
    click(w, 10 + 90, 10 + body.y + 2);
    w.render();
    CHECK(screen.px(10 + 90, 10 + body.y + 2) == theme.selection);   // focused: the selection colour
  }
  CHECK(a.live == 0);
}

// --- ImageView -----------------------------------------------------------------

struct Picture {
  TestAllocator& a;
  graphics::Surface surface;
  Picture(TestAllocator& alloc, uint32_t w, uint32_t h, uint32_t color) : a(alloc) {
    surface.pixels = allocate_view<uint32_t>(a, w * h);
    surface.width = w; surface.height = h; surface.pitch = w;
    for (uint32_t& p : surface.pixels) p = color;
  }
  ~Picture() { release_view(a, surface.pixels); }
  void set(uint32_t x, uint32_t y, uint32_t c) { surface.row(y)[x] = c; }
};

static void test_image_view() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 400, 300);
    Window w(a, screen.renderer, theme, {400, 300});
    ImageView v(a);
    v.set_bounds({10, 10, 200, 150});   // a 196 x 146 viewport
    w.add(&v);
    Counter zooms;
    v.on_zoom_changed(zooms.handler());
    w.layout();
    w.render();
    CHECK(v.image() == nullptr);

    // A small picture: left half red, right half blue. Shown at 100%, centred; the rest is grey.
    Picture small(a, 40, 20, 0xFFFF0000);
    for (uint32_t y = 0; y < 20; ++y)
      for (uint32_t x = 20; x < 40; ++x) small.set(x, y, 0xFF0000FF);
    v.set_image(&small.surface);
    CHECK(v.fitted() && v.zoom_percent() == 100 && v.scroll_position().x == -(196 - 40) / 2 && v.scroll_position().y == -(146 - 20) / 2);
    w.render();
    const int32_t left = 10 + 2 + (196 - 40) / 2, top = 10 + 2 + (146 - 20) / 2;
    CHECK(screen.px(left + 5, top + 5) == 0xFFFF0000 && screen.px(left + 35, top + 15) == 0xFF0000FF);
    CHECK(screen.px(left - 3, top + 5) == 0xFF808080 && screen.px(left + 5, top + 25) == 0xFF808080);

    // Zoom in: pixels repeat. 400% makes it 160 x 80.
    v.set_zoom(400);
    CHECK(!v.fitted() && v.zoom_percent() == 400 && zooms.count == 1);
    w.render();
    const int32_t left4 = 10 + 2 + (196 - 160) / 2, top4 = 10 + 2 + (146 - 80) / 2;
    CHECK(screen.px(left4 + 70, top4 + 10) == 0xFFFF0000 && screen.px(left4 + 90, top4 + 10) == 0xFF0000FF);
    CHECK(screen.px(left4 - 2, top4 + 10) == 0xFF808080);

    // Larger than the view: it pans (dragging moves the picture with the pointer) and stays covering the view.
    v.set_zoom(1600);   // 640 x 320
    CHECK(v.scroll_position().x == (640 - 196) / 2 && v.scroll_position().y == (320 - 146) / 2);
    w.mouse_move(100, 80);
    w.mouse_button(100, 80, MouseButton::Left, true);
    w.mouse_move(90, 70);   // the pointer went up-left: we look further down-right
    CHECK(v.scroll_position().x == (640 - 196) / 2 + 10 && v.scroll_position().y == (320 - 146) / 2 + 10);
    w.mouse_move(-500, -500);
    CHECK(v.scroll_position().x == 640 - 196 && v.scroll_position().y == 320 - 146);
    w.mouse_move(700, 700);
    CHECK(v.scroll_position().x == 0 && v.scroll_position().y == 0);
    w.mouse_button(700, 700, MouseButton::Left, false);
    w.mouse_move(100, 80);
    CHECK(v.scroll_position().x == 0);   // no longer dragging

    // Wheel: the next step up or down, around the pointer.
    v.set_zoom(100);
    w.mouse_wheel(100, 80, 1);
    CHECK(v.zoom_percent() == 150);
    w.mouse_wheel(100, 80, -1);
    w.mouse_wheel(100, 80, -1);
    CHECK(v.zoom_percent() == 75);
    v.zoom_in();
    v.zoom_in();
    CHECK(v.zoom_percent() == 150);
    v.zoom_out();
    CHECK(v.zoom_percent() == 100);
    v.set_zoom(5000);
    CHECK(v.zoom_percent() == ImageView::kMaxZoom);
    v.set_zoom(0);
    CHECK(v.zoom_percent() == ImageView::kMinZoom);

    // Keys: + - 0 F and the arrows.
    v.set_zoom(100);
    click(w, 100, 80);   // focus
    type_char(w, '+');
    CHECK(v.zoom_percent() == 150);
    type_char(w, '-');
    type_char(w, '-');
    CHECK(v.zoom_percent() == 75);
    type_char(w, '0');
    CHECK(v.zoom_percent() == 100 && !v.fitted());
    type_char(w, 'f');
    CHECK(v.fitted());
    v.set_zoom(1600);
    press(w, key::kLeft);
    CHECK(v.scroll_position().x == (640 - 196) / 2 - 24);
    press(w, key::kDown);
    CHECK(v.scroll_position().y == (320 - 146) / 2 + 24);

    // A big picture is fitted by shrinking, and stays fitted when the view changes size.
    Picture big(a, 1000, 500, 0xFF00FF00);
    v.set_image(&big.surface);
    CHECK(v.fitted() && v.zoom_percent() == 19);
    v.set_bounds({10, 10, 300, 250});
    w.layout();
    CHECK(v.fitted() && v.zoom_percent() == 29);   // (296 x 246 viewport: 296% / ... = 29)
    w.render();
    CHECK(screen.px(10 + 2 + 100, 10 + 2 + 123) == 0xFF00FF00);
    v.set_image(nullptr);
    w.render();
    v.set_image(&small.surface);

    // Zooming out averages the pixels: one-pixel stripes of black and white come out grey.
    Picture stripes(a, 64, 64, 0xFF000000);
    for (uint32_t y = 0; y < 64; ++y)
      for (uint32_t x = 0; x < 64; x += 2) stripes.set(x, y, 0xFFFFFFFF);
    v.set_image(&stripes.surface);
    v.set_zoom(50);
    w.render();
    const uint32_t grey = screen.px(10 + 2 + 98, 10 + 2 + 98) & 0xFF;   // in the middle of the 32 x 32 picture
    CHECK(grey > 0x70 && grey < 0x90);

    // Transparency shows the chequerboard; changing pixels in place needs image_changed().
    Picture ghost(a, 16, 16, 0x00000000);
    v.set_image(&ghost.surface);
    v.set_zoom(100);
    w.render();
    const int32_t gx = 10 + 2 + (static_cast<int32_t>(v.bounds().width) - 4 - 16) / 2, gy = 10 + 2 + (static_cast<int32_t>(v.bounds().height) - 4 - 16) / 2;
    const uint32_t c1 = screen.px(gx + 1, gy + 1), c2 = screen.px(gx + 9, gy + 1);
    CHECK((c1 == 0xFFFFFFFF && c2 == 0xFFC0C0C0) || (c1 == 0xFFC0C0C0 && c2 == 0xFFFFFFFF));
    ghost.set(1, 1, 0xFF123456);
    w.render();
    CHECK(screen.px(gx + 1, gy + 1) == c1);   // the cache does not know yet
    v.image_changed();
    w.render();
    CHECK(screen.px(gx + 1, gy + 1) == 0xFF123456);
    ghost.set(2, 1, 0x80FF0000);   // half-transparent red over the chequerboard
    v.image_changed();
    w.render();
    const uint32_t mixed = screen.px(gx + 2, gy + 1);
    CHECK(((mixed >> 16) & 0xFF) > 0xD0 && ((mixed >> 8) & 0xFF) > 0x50 && ((mixed >> 8) & 0xFF) < 0x90);
  }
  CHECK(a.live == 0);
}

// --- AudioPlayerControl -------------------------------------------------------------

struct FakeDevice final : hardware::IAudioDevice {
  uint32_t queued = 0;
  uint8_t vol = 80; bool mute = false;
  const char* name() const override { return "fake audio"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    uint32_t n = pcm.size() < 16384 - queued ? (uint32_t)pcm.size() : 16384 - queued;
    n -= n % hardware::kAudioFrameSize;
    queued += n;
    return n;
  }
  uint32_t queued_bytes() override { return queued; }
  uint32_t free_bytes() override { return 16384 - queued; }
  void play_all() { queued = 0; }
  void set_volume(uint8_t v) override { vol = v; }
  uint8_t volume() const override { return vol; }
  void set_muted(bool m) override { mute = m; }
  bool muted() const override { return mute; }
  hardware::AudioStatistics statistics() override { return {}; }
};

static int16_t g_two_seconds[96000 * 2];

static void test_player_control() {
  for (int i = 0; i < 96000; ++i) g_two_seconds[2 * i] = g_two_seconds[2 * i + 1] = (int16_t)((i % 100) * 200 - 10000);
  TestAllocator a;
  Theme theme;
  FakeDevice dev;
  audio::AudioMixer mixer(dev, a);
  CHECK(mixer.initialize());
  {
    audio::AudioPlayer player(mixer, a);
    Screen screen(a, 400, 200);
    Window w(a, screen.renderer, theme, {400, 200});
    AudioPlayerControl panel(a, player);
    panel.set_bounds({10, 10, 320, AudioPlayerControl::kHeight});
    w.add(&panel);
    w.layout();
    auto step = [&](int rounds) { for (int i = 0; i < rounds; ++i) { player.update(); mixer.pump(); dev.play_all(); panel.refresh(); } };

    // Nothing loaded: the parts are there but disabled.
    CHECK(!panel.play_button().enabled() && !panel.stop_button().enabled() && !panel.seek_slider().enabled());
    CHECK(panel.time_text().equals("00:00 / 00:00"));
    click_center(w, panel.play_button());
    CHECK(player.state() == audio::PlayerState::Empty);

    audio::MemoryPcmSource source(ConstByteView((const uint8_t*)g_two_seconds, sizeof g_two_seconds), audio::StreamFormat());
    CHECK(player.open(&source));
    panel.refresh();
    CHECK(panel.play_button().enabled() && panel.seek_slider().enabled() && panel.time_text().equals("00:00 / 00:02"));
    CHECK(panel.play_button().text().equals("Play"));

    // Play: the button turns into Pause, the slider and the clock move, the meter shows the sound.
    click_center(w, panel.play_button());
    CHECK(player.playing() && panel.play_button().text().equals("Pause"));
    step(12);
    CHECK(panel.seek_slider().value() > 0);
    CHECK(panel.meter().level() > 50);   // a loud tone
    w.render();

    // Pause keeps the place.
    click_center(w, panel.play_button());
    CHECK(player.state() == audio::PlayerState::Paused && panel.play_button().text().equals("Play"));
    const uint64_t at = player.position();
    step(5);
    CHECK(player.position() == at);
    step(25);
    CHECK(panel.meter().level() == 0);   // the meter falls back with no sound

    // Seeking with the slider: a click at three quarters of the track.
    const Point track = panel.seek_slider().window_origin();
    click(w, track.x + (int32_t)panel.seek_slider().bounds().width * 3 / 4, track.y + 12);
    CHECK(near((int32_t)(player.position() * 1000 / player.duration()), 750, 40));
    panel.refresh();
    CHECK(panel.time_text().equals("00:01 / 00:02"));

    // Stop: back to the start.
    click_center(w, panel.stop_button());
    CHECK(player.state() == audio::PlayerState::Stopped && player.position() == 0 && panel.seek_slider().value() == 0);
    CHECK(panel.time_text().equals("00:00 / 00:02"));

    // The volume slider changes the player's volume; changes made through the player show on the slider.
    const Point vol = panel.volume_slider().window_origin();
    click(w, vol.x + 10, vol.y + 12);
    CHECK(player.volume() < 20);
    player.set_volume(90);
    panel.refresh();
    CHECK(panel.volume_slider().value() == 90);

    // Played to the end by itself: the clock shows the full length and the button says Play again.
    player.seek(95000);
    player.play();
    for (int i = 0; i < 40 && player.playing(); ++i) step(1);
    panel.refresh();
    CHECK(player.state() == audio::PlayerState::Stopped && panel.time_text().equals("00:02 / 00:02") && panel.play_button().text().equals("Play"));
    CHECK(panel.seek_slider().value() == 1000);

    player.close();
    panel.refresh();
    CHECK(!panel.play_button().enabled() && panel.time_text().equals("00:00 / 00:00"));
  }
  mixer.pump();   // the closed stream is let go
  CHECK(mixer.stream_count() == 0);
}

int main() {
  test_scroll_bar();
  test_data_grid();
  test_data_grid_scrolling();
  test_grid_rendering();
  test_image_view();
  test_player_control();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gui views: all checks passed\n");
  return 0;
}
