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
#include "Gui/Color.hpp"
#include "Kernel/Clipboard.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Desktop/FileExplorer.hpp"
#include "Desktop/Notepad.hpp"
#include "Desktop/Paint.hpp"
#include "Desktop/TaskManager.hpp"
#include "Gui/Controls/TextEditor.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Gui/Controls/BreadcrumbBar.hpp"
#include "Gui/Controls/ColorPalette.hpp"
#include "Gui/Controls/DrawingCanvas.hpp"
#include "Gui/Controls/ToolStrip.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/ImageView.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Gui/Controls/ScrollBar.hpp"
#include "Gui/Controls/TabControl.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Controls/VideoPlayerControl.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"
#include "Video/AviWriter.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/AllocatedView.hpp"

using namespace gui;
namespace key = hardware::key;
namespace modifier = hardware::modifier;

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

// Chooses a menu item the way a user does: clicks the title, then the item's row in the drop-down. False if there is no such
// menu or item, or the item is disabled (the click then does nothing).
static bool choose(Window& w, MenuBar& bar, const char* menu, const char* item) {
  int32_t index = -1;
  for (uint32_t m = 0; m < bar.menu_count(); ++m)
    if (bar.menu(m)->title().equals(menu)) index = static_cast<int32_t>(m);
  if (index < 0 || !bar.find_item(menu, item)) return false;
  bar.open_menu(index);
  Control* popup = w.popup();
  if (!popup) return false;
  int32_t top = 2;
  Menu* m = bar.menu(static_cast<uint32_t>(index));
  for (uint32_t i = 0; i < m->item_count(); ++i) {
    const int32_t height = m->item(i)->separator ? MenuPopup::kSeparatorHeight : MenuPopup::kRowHeight;
    if (!m->item(i)->separator && m->item(i)->text.view().equals(item)) {
      const bool enabled = m->item(i)->enabled();
      click(w, popup->bounds().x + 30, popup->bounds().y + top + height / 2);
      if (!enabled) bar.close_menu();   // a disabled item keeps the menu open
      return enabled;
    }
    top += height;
  }
  return false;
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

// --- VideoPlayerControl --------------------------------------------------------------

struct FixedClock final : hardware::IClock {
  uint64_t now = 0;
  const char* name() const override { return "test clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return now; }
};

struct NoPci final : hardware::IPciBus {
  const char* name() const override { return "no pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 0; }
  bool pci_device(uint32_t, hardware::PciDeviceInfo*) const override { return false; }
  uint32_t read32(hardware::PciAddress, uint16_t) const override { return 0; }
  void write32(hardware::PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(hardware::PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(hardware::PciAddress, uint8_t) const override { return 0; }
};

// A clip of `frames` frames at 10 fps; frame i is a flat colour with red level 10 * i + 5.
static bool make_video_clip(TestAllocator& a, video::AviWriter& writer, uint32_t frames, bool with_audio) {
  using namespace hardware;
  const uint32_t w = 64, h = 36;
  audio::StreamFormat audio_format{8000, 1, audio::SampleType::U8};
  if (!writer.begin(VideoCodec::Mjpeg, w, h, 10, 1, PixelFormat::I420, with_audio ? &audio_format : nullptr)) return false;
  SoftwareVideoCodec device(a);
  EncoderConfig ec;
  ec.codec = VideoCodec::Mjpeg; ec.width = w; ec.height = h; ec.input = PixelFormat::Bgra8888; ec.quality = 90;
  UniquePointer<IVideoEncoder> enc = device.create_encoder(ec);
  if (!enc) return false;
  for (uint32_t i = 0; i < frames; ++i) {
    VideoFrameBuffer frame(a);
    if (!frame.allocate({w, h, PixelFormat::Bgra8888})) return false;
    uint32_t* p = reinterpret_cast<uint32_t*>(frame.plane(0).data());
    for (uint32_t k = 0; k < w * h; ++k) p[k] = 0xFF000000 | ((10 * i + 5) << 16) | (80 << 8) | 60;
    VideoPacket packet;
    if (enc->submit(frame.frame()) != SubmitResult::Accepted || !enc->receive(&packet) || !writer.add_frame(packet.data, true)) return false;
    if (with_audio) {
      uint8_t samples[800];
      for (uint32_t k = 0; k < 800; ++k) samples[k] = (uint8_t)(128 + (k % 64) - 32);
      if (!writer.add_audio(ConstByteView(samples, 800))) return false;
    }
  }
  return writer.finish();
}

static void test_video_control() {
  TestAllocator a;
  Theme theme;
  FakeDevice dev;
  audio::AudioMixer mixer(dev, a);
  CHECK(mixer.initialize());
  {
    NoPci pci;
    hardware::SystemVideoCodecs codecs(pci, a);
    codecs.initialize();
    FixedClock clock;
    video::AviWriter clip(a);
    CHECK(make_video_clip(a, clip, 20, true));
    video::VideoPlayer player(clock, codecs, a, &mixer);
    Screen screen(a, 520, 400);
    Window w(a, screen.renderer, theme, {520, 400});
    VideoPlayerControl panel(a, player);
    panel.set_bounds({10, 10, 480, 360});
    w.add(&panel);
    w.layout();
    auto step = [&](int rounds) { for (int i = 0; i < rounds; ++i) { player.update(); mixer.pump(); dev.play_all(); panel.refresh(); } };

    // Nothing open: disabled controls, no picture.
    CHECK(!panel.play_button().enabled() && !panel.seek_slider().enabled() && !panel.volume_slider().enabled());
    CHECK(panel.view().image() == nullptr && panel.time_text().equals("00:00 / 00:00"));
    w.render();

    CHECK(player.open(clip.file()) == video::OpenResult::Ok);
    panel.refresh();
    CHECK(panel.play_button().enabled() && panel.seek_slider().enabled() && panel.volume_slider().enabled());
    CHECK(panel.view().image() != nullptr && panel.view().fitted() && panel.time_text().equals("00:00 / 00:02"));
    w.layout();
    // The picture fills the view: it is enlarged (64 x 36 into 476 x 298 is x7.4, bounded by the width).
    CHECK(panel.view().zoom_percent() > 400);
    w.render();
    const Rect2D vp = panel.view().viewport();
    const int32_t cx = 10 + vp.x + (int32_t)vp.width / 2, cy = 10 + vp.y + (int32_t)vp.height / 2;
    CHECK(near((int)((screen.px(cx, cy) >> 16) & 0xFF), 5, 6));   // frame 0

    // Play: the button says Pause, the picture and the slider follow the sound.
    click_center(w, panel.play_button());
    CHECK(player.playing() && panel.play_button().text().equals("Pause"));
    step(12);
    CHECK(panel.seek_slider().value() > 0);
    w.render();
    const int red = (int)((screen.px(cx, cy) >> 16) & 0xFF);
    CHECK(red >= 5 + 10 * (int)player.current_frame() - 6 && red <= 5 + 10 * (int)player.current_frame() + 6 && player.current_frame() > 0);

    click_center(w, panel.play_button());   // pause
    CHECK(player.state() == video::VideoState::Paused && panel.play_button().text().equals("Play"));
    const int64_t at = player.position_us();
    step(5);
    CHECK(player.position_us() == at);

    // Seek by clicking the slider at 3/4.
    const Point track = panel.seek_slider().window_origin();
    click(w, track.x + (int32_t)panel.seek_slider().bounds().width * 3 / 4, track.y + 12);
    CHECK(near((int)(player.position_us() / 1000), 1500, 90) && player.current_frame() >= 13 && player.current_frame() <= 16);
    panel.refresh();
    CHECK(panel.time_text().equals("00:01 / 00:02"));
    w.render();
    const int red_after = (int)((screen.px(cx, cy) >> 16) & 0xFF);
    CHECK(red_after >= 5 + 10 * (int)player.current_frame() - 6 && red_after <= 5 + 10 * (int)player.current_frame() + 6);

    // Stop: back to the start; the picture is the first one again.
    click_center(w, panel.stop_button());
    CHECK(player.state() == video::VideoState::Stopped && player.position_us() == 0 && panel.seek_slider().value() == 0);
    w.render();
    CHECK(near((int)((screen.px(cx, cy) >> 16) & 0xFF), 5, 6));

    // Volume and loop.
    const Point vol = panel.volume_slider().window_origin();
    click(w, vol.x + 8, vol.y + 12);
    CHECK(player.audio()->volume() < 25);
    click_center(w, panel.loop_box());
    CHECK(player.looping() && panel.loop_box().checked());
    click_center(w, panel.loop_box());
    CHECK(!player.looping());

    // The viewer's own controls still work on the picture: zoom with the wheel makes it leave fit mode.
    panel.view().set_zoom(100);
    CHECK(!panel.view().fitted());
    panel.view().fit();
    CHECK(panel.view().fitted());

    player.close();
    panel.refresh();
    CHECK(!panel.play_button().enabled() && panel.view().image() == nullptr && panel.time_text().equals("00:00 / 00:00"));
    w.render();
  }
  mixer.pump();
  CHECK(mixer.stream_count() == 0);
}


// --- ListView and the file explorer ---------------------------------------------------------------------------

static void fill_list(ListView& list, uint32_t count, uint32_t folders) {
  list.add_column("Name", 120);
  list.add_column("Size", 60, Align::Right);
  for (uint32_t i = 0; i < count; ++i) {
    char name[16];
    snprintf(name, sizeof name, "item%02u", i);
    const StringView cells[2] = {name, "1 KB"};
    list.add_item(cells, 2, i < folders ? ItemIcon::Folder : ItemIcon::File);
  }
}

static void test_list_view() {
  TestAllocator a;
  {
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    ListView list(a);
    list.set_bounds({10, 10, 300, 100});
    w.add(&list);
    CHECK(list.add_item(nullptr, 0, ItemIcon::File) == -1);  // no columns yet
    fill_list(list, 20, 3);
    CHECK(list.add_column("Late", 40) == -1);                // not once there are items
    CHECK(list.item_count() == 20 && list.column_count() == 2 && list.item_icon(2) == ItemIcon::Folder && list.item_icon(3) == ItemIcon::File);
    w.layout();
    Counter selections, activations;
    list.on_selection_changed(selections.handler());
    list.on_item_activated(activations.handler());
    uint64_t now = 1000000;
    list.set_clock(&now);

    // Details: a row per item under the header, a vertical scroll bar because they do not all fit.
    list.set_view_mode(ViewMode::Details);
    w.layout();
    Rect2D r0, r1;
    CHECK(list.item_rect(0, &r0) && list.item_rect(1, &r1) && r1.y - r0.y == ListView::kRowHeight && r0.y == ListView::kBorder + ListView::kHeaderHeight);
    CHECK(list.vertical_scroll_bar().visible() && !list.horizontal_scroll_bar().visible());
    click(w, 10 + r1.x + 20, 10 + r1.y + 5);
    CHECK(list.selected_item() == 1 && selections.count == 1 && activations.count == 0);
    now += 100000;
    click(w, 10 + r1.x + 20, 10 + r1.y + 5);   // a second click within the interval
    CHECK(activations.count == 1);
    now += 100000;
    click(w, 10 + r1.x + 20, 10 + r1.y + 5);   // the double click was used up: this starts a new one
    CHECK(activations.count == 1);
    now += 900000;
    click(w, 10 + r1.x + 20, 10 + r1.y + 5);   // too slow
    CHECK(activations.count == 1);
    press(w, key::kDown);
    CHECK(list.selected_item() == 2);
    press(w, key::kEnter);
    CHECK(activations.count == 2);
    press(w, 0x4D);                            // End
    CHECK(list.selected_item() == 19 && list.scroll_y() > 0);
    Rect2D last;
    CHECK(list.item_rect(19, &last) && last.y >= list.body_rect().y && last.y + static_cast<int32_t>(last.height) <= list.body_rect().y + static_cast<int32_t>(list.body_rect().height));
    press(w, 0x4A);                            // Home
    CHECK(list.selected_item() == 0 && list.scroll_y() == 0);
    w.mouse_wheel(60, 60, -1);
    CHECK(list.scroll_y() == 3 * ListView::kRowHeight);
    w.render();

    // Dragging the border at a header's right edge resizes the column.
    w.mouse_move(10 + 2 + 120, 10 + 2 + 5);
    w.mouse_button(10 + 2 + 120, 10 + 2 + 5, MouseButton::Left, true);
    w.mouse_move(10 + 2 + 150, 10 + 2 + 5);
    w.mouse_button(10 + 2 + 150, 10 + 2 + 5, MouseButton::Left, false);
    CHECK(list.column_width(0) == 150);

    // List: top to bottom, then left to right, with a horizontal scroll bar.
    list.set_view_mode(ViewMode::List);
    w.layout();
    CHECK(list.scroll_x() == 0 && list.scroll_y() == 0 && list.horizontal_scroll_bar().visible() && !list.vertical_scroll_bar().visible());
    Rect2D a0, a1, a4;
    const int32_t per = list.body_rect().height / ListView::kRowHeight;
    CHECK(per >= 3 && list.item_rect(0, &a0) && list.item_rect(1, &a1) && list.item_rect(static_cast<uint32_t>(per), &a4));
    CHECK(a1.x == a0.x && a1.y - a0.y == ListView::kRowHeight && a4.x > a0.x && a4.y == a0.y);
    list.select_item(0);
    press(w, key::kRight);
    CHECK(list.selected_item() == per);
    press(w, key::kDown);
    CHECK(list.selected_item() == per + 1);
    press(w, key::kLeft);
    CHECK(list.selected_item() == 1);
    list.select_item(19);
    Rect2D end;
    CHECK(list.item_rect(19, &end) && end.x >= 0 && end.x < 300 && list.scroll_x() > 0);
    click(w, 10 + end.x + 20, 10 + end.y + 5);
    CHECK(list.selected_item() == 19);
    w.render();

    // Icons: a grid that wraps to the width, scrolling vertically.
    list.set_view_mode(ViewMode::Icons);
    w.layout();
    list.select_item(0);
    CHECK(list.vertical_scroll_bar().visible() && !list.horizontal_scroll_bar().visible());
    Rect2D i0, i1, i3;
    CHECK(list.item_rect(0, &i0) && list.item_rect(1, &i1) && list.item_rect(3, &i3));
    CHECK(i1.x - i0.x == ListView::kIconCellWidth && i1.y == i0.y && i3.x == i0.x && i3.y - i0.y == ListView::kIconCellHeight);
    press(w, key::kDown);
    CHECK(list.selected_item() == 3);
    press(w, key::kRight);
    CHECK(list.selected_item() == 4);
    click(w, 10 + i1.x + 40, 10 + i1.y + 20);
    CHECK(list.selected_item() == 1);
    click(w, 10 + 290, 10 + 90);               // not on an item: nothing selected
    w.render();
    list.select_item(19);
    w.render();
    CHECK(list.scroll_y() > 0);

    // Switching back and clearing.
    list.set_view_mode(ViewMode::Details);
    list.clear_items();
    CHECK(list.item_count() == 0 && list.selected_item() == -1 && list.scroll_y() == 0 && !list.vertical_scroll_bar().visible());
    w.render();
  }
  CHECK(a.live == 0);
}

static void make_file(filesystem::IFileSystem& fs, const char* path, const char* text) {
  UniquePointer<filesystem::IFile> file;
  uint64_t written = 0;
  CHECK(fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok);
  CHECK(file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text), strlen(text)), &written) == filesystem::Status::Ok && written == strlen(text));
}

static void test_file_explorer() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    CHECK(fs.create_directory("/docs") == filesystem::Status::Ok);
    CHECK(fs.create_directory("/Apps") == filesystem::Status::Ok);
    CHECK(fs.create_directory("/docs/old") == filesystem::Status::Ok);
    make_file(fs, "/readme", "hello");
    make_file(fs, "/b.txt", "12345678901");
    make_file(fs, "/a.bin", "x");
    make_file(fs, "/docs/note.md", "# hi");

    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::FileExplorer ex(a);
    uint64_t now = 0;
    ex.set_clock(&now);
    ex.set_file_system(nullptr);
    CHECK(ex.status_label().text().equals("No storage available") && ex.view().item_count() == 0);
    ex.set_file_system(&fs);
    CHECK(mgr.add_window(&ex));
    w.layout();

    auto name_of = [&](uint32_t i) {
      String s(a);
      ex.view().item_text(i, 0, &s);
      return s;
    };
    auto cell_of = [&](uint32_t i, uint32_t column) {
      String s(a);
      ex.view().item_text(i, column, &s);
      return s;
    };
    // Folders first, then files, each by name without regard to case.
    CHECK(ex.path().equals("/") && ex.view().item_count() == 5);
    CHECK(name_of(0).view().equals("Apps") && name_of(1).view().equals("docs") && name_of(2).view().equals("a.bin") &&
          name_of(3).view().equals("b.txt") && name_of(4).view().equals("readme"));
    CHECK(ex.view().item_icon(1) == ItemIcon::Folder && ex.view().item_icon(2) == ItemIcon::File);
    CHECK(cell_of(3, 1).view().equals("11 B") && cell_of(3, 2).view().equals("TXT File") && cell_of(4, 2).view().equals("File") && cell_of(0, 2).view().equals("Folder") && cell_of(0, 1).empty());
    CHECK(ex.status_label().text().equals("5 items"));
    CHECK(ex.address_bar().path().equals("/") && ex.address_bar().segment_count() == 1 && ex.details_item()->checked() && ex.view().view_mode() == ViewMode::Details);

    // The three views are chosen with the toolbar's buttons.
    CHECK(choose(w, ex.menu_bar(), "View", "List"));
    CHECK(ex.view().view_mode() == ViewMode::List && !ex.details_item()->checked() && ex.list_item()->checked());
    CHECK(choose(w, ex.menu_bar(), "View", "Icons"));
    CHECK(ex.view().view_mode() == ViewMode::Icons);
    w.layout();
    w.render();
    ex.set_view_mode(ViewMode::Details);
    CHECK(ex.view().view_mode() == ViewMode::Details && ex.details_item()->checked() && !ex.icons_item()->checked());

    // A file is passed on, not opened as a folder.
    Counter opened;
    ex.on_file_opened(opened.handler());
    w.set_focus(&ex.view());
    ex.view().select_item(3);
    press(w, key::kEnter);
    CHECK(opened.count == 1 && ex.opened_file().equals("/b.txt") && ex.path().equals("/"));
    {
      kernel::Clipboard cb(a);                  // Ctrl+C puts the selected item's full path on the clipboard
      w.set_clipboard(&cb);
      press(w, key::kA + 2, modifier::kLeftControl);
      String copied(a);
      CHECK(cb.get_text(&copied) && copied.view().equals("/b.txt") && ex.status_label().text().equals("Copied /b.txt"));
      w.set_clipboard(nullptr);
    }

    // Opening a folder: Enter on it, a double click, then Up and Backspace go back.
    w.set_focus(&ex.view());
    ex.view().select_item(1);
    CHECK(ex.status_label().text().equals("5 items  |  docs"));
    press(w, key::kEnter);
    CHECK(ex.path().equals("/docs") && ex.view().item_count() == 2 && name_of(0).view().equals("old") && name_of(1).view().equals("note.md"));
    CHECK(ex.address_bar().path().equals("/docs") && ex.address_bar().segment_count() == 2 && ex.view().selected_item() == -1);
    CHECK(ex.status_label().text().equals("2 items"));
    ex.view().select_item(1);
    CHECK(ex.status_label().text().equals("2 items  |  note.md, 4 B"));
    press(w, key::kEnter);                      // a file: nothing opens
    CHECK(ex.path().equals("/docs"));
    // Clicking the root in the address bar goes back there.
    {
      const Point bar = ex.address_bar().window_origin();
      const BreadcrumbBar::Segment root = ex.address_bar().segment(0);
      click(w, bar.x + root.x + 4, bar.y + 12);
    }
    CHECK(ex.path().equals("/") && ex.view().item_count() == 5 && ex.address_bar().segment_count() == 1);
    // Typing a path: click after the last folder, type, Enter; a path that does not exist is refused.
    {
      const Point bar = ex.address_bar().window_origin();
      const uint32_t width = ex.address_bar().bounds().width;
      click(w, bar.x + static_cast<int32_t>(width) - 6, bar.y + 12);
      CHECK(ex.address_bar().editing() && w.focus() == &ex.address_bar().edit_box());
      ex.address_bar().edit_box().set_text("/docs/old");
      press(w, key::kEnter);
      CHECK(!ex.address_bar().editing() && ex.path().equals("/docs/old") && ex.address_bar().segment_count() == 3);
      ex.address_bar().begin_edit();
      ex.address_bar().edit_box().set_text("/nowhere");
      press(w, key::kEnter);
      CHECK(ex.path().equals("/docs/old") && ex.status_label().text().equals("Cannot open /nowhere"));
      press(w, key::kA + 11, modifier::kLeftControl);   // Ctrl+L, from anywhere in the window
      CHECK(ex.address_bar().editing());
      press(w, key::kEscape);
      CHECK(!ex.address_bar().editing());
      CHECK(ex.go_up() && ex.go_up() && ex.path().equals("/"));
      w.set_focus(&ex.view());
    }
    Rect2D row;
    CHECK(ex.view().item_rect(1, &row));
    const Point origin = ex.view().window_origin();
    now = 5000000;
    click(w, origin.x + row.x + 30, origin.y + row.y + 5);
    now += 200000;
    click(w, origin.x + row.x + 30, origin.y + row.y + 5);
    CHECK(ex.path().equals("/docs"));
    ex.view().select_item(0);
    press(w, key::kEnter);
    CHECK(ex.path().equals("/docs/old") && ex.view().item_count() == 0);
    press(w, key::kBackspace);
    CHECK(ex.path().equals("/docs"));
    CHECK(ex.go_up() && ex.path().equals("/") && !ex.go_up());
    CHECK(!ex.open_folder("/missing") && ex.path().equals("/"));

    // refresh() reads the folder again: a new file shows up, and a folder that has vanished is replaced by the nearest one above it that is there
    CHECK(ex.refresh() && ex.view().item_count() == 5);
    make_file(fs, "/c.txt", "new");
    CHECK(ex.refresh() && ex.view().item_count() == 6 && ex.path().equals("/"));
    CHECK(ex.open_folder("/docs/old") && ex.path().equals("/docs/old"));
    CHECK(fs.remove("/docs/old") == filesystem::Status::Ok);
    CHECK(ex.refresh() && ex.path().equals("/docs") && ex.view().item_count() == 1);
    CHECK(fs.remove("/docs/note.md") == filesystem::Status::Ok && fs.remove("/docs") == filesystem::Status::Ok);
    CHECK(ex.refresh() && ex.path().equals("/") && ex.view().item_count() == 5);
    desktop::FileExplorer detached(a);
    CHECK(!detached.refresh());
    w.render();
    mgr.remove_window(&ex);
  }
  CHECK(a.live == 0);
}

// --- Task manager ----------------------------------------------------------------------------------------------

struct FakeMonitor final : desktop::ISystemMonitor {
  uint64_t core_busy[2] = {0, 0}, core_idle[2] = {0, 0};
  uint64_t worker_ticks = 0, idle_ticks[2] = {0, 0};
  bool second_thread = true;
  uint64_t free_bytes = 512ull << 20;
  uint32_t ticks_per_second() const override { return 1000; }
  uint32_t core_count() const override { return 3; }
  bool core(uint32_t i, desktop::TaskCore* out) override {
    if (i >= 3) return false;
    *out = desktop::TaskCore();
    out->online = i < 2;
    out->efficiency = i == 1;
    if (i < 2) { out->busy_ticks = core_busy[i]; out->idle_ticks = core_idle[i]; }
    return true;
  }
  uint32_t threads(desktop::TaskThread* out, uint32_t capacity) override {
    uint32_t n = 0;
    auto put = [&](uint64_t id, const char* name, desktop::TaskState state, bool idle, int32_t core, uint64_t ticks) {
      if (n >= capacity) return;
      out[n] = desktop::TaskThread();
      out[n].id = id;
      strncpy(out[n].name, name, sizeof out[n].name - 1);
      out[n].state = state; out[n].idle = idle; out[n].core = core; out[n].cpu_ticks = ticks;
      ++n;
    };
    put(1, "main", desktop::TaskState::Blocked, false, -1, 5);
    put(0, "idle", desktop::TaskState::Running, true, 0, 0);
    if (second_thread) put(7, "worker", desktop::TaskState::Running, false, 1, worker_ticks);
    return n;
  }
  desktop::TaskMemory memory() override { return {1ull << 30, free_bytes}; }
};

static void test_task_manager() {
  TestAllocator a;
  {
    FakeMonitor mon;
    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::TaskManager tm(a);
    tm.update(1000000);
    CHECK(tm.info_label().text().equals("No system monitor available"));
    tm.set_monitor(&mon);
    CHECK(mgr.add_window(&tm));
    w.layout();

    tm.update(1000000);   // first sample: no load yet, but memory and the threads are there
    CHECK(tm.grid().row_count() == 3 && tm.cpu_label().text().equals("0%"));
    CHECK(tm.memory_label().text().equals("512.0 MB / 1.0 GB") && tm.memory_meter().level() == 50);
    CHECK(tm.info_label().text().equals("Threads: 3   Cores: 2   Uptime: 00:00:01"));
    CHECK(tm.core_meter(0) && tm.core_meter(1) && !tm.core_meter(2) && tm.core_label(0)->text().equals("0%"));
    CHECK(tm.tabs().page_count() == 2 && tm.tabs().page_title(0).equals("Processes") && tm.tabs().page_title(1).equals("Performance") && tm.tabs().selected_page() == 0);

    // One second later: core 0 was 25% busy, core 1 75% busy, and the worker ran 600 of the 1000 ticks.
    mon.core_busy[0] = 250; mon.core_idle[0] = 750;
    mon.core_busy[1] = 750; mon.core_idle[1] = 250;
    mon.worker_ticks = 600;
    mon.free_bytes = 256ull << 20;
    tm.update(2000000);
    CHECK(tm.cpu_label().text().equals("50%") && tm.cpu_meter().level() == 50);
    CHECK(tm.core_meter(0)->level() == 25 && tm.core_label(0)->text().equals("25%") && tm.core_meter(1)->level() == 75 && tm.core_label(1)->text().equals("75%"));
    CHECK(tm.grid().visible() && tm.tabs().page(0)->visible() && !tm.tabs().page(1)->visible());
    tm.tabs().select_page(1);                   // the other tab shows the meters instead of the table
    w.layout();
    w.render();
    CHECK(!tm.tabs().page(0)->visible() && tm.tabs().page(1)->visible() && tm.cpu_meter().bounds().width > 100 && tm.core_meter(1)->bounds().width > 100);
    tm.tabs().select_page(0);
    CHECK(tm.memory_label().text().equals("768.0 MB / 1.0 GB") && tm.memory_meter().level() == 75);
    CHECK(tm.thread_load_tenths(7) == 600 && tm.thread_load_tenths(1) == 0 && tm.thread_load_tenths(99) == -1);
    String cell(a);
    CHECK(tm.grid().row_count() == 3);
    // Row 2 is the worker: id, name, state, core, CPU %, CPU seconds.
    int32_t row = -1;
    for (uint32_t i = 0; i < 3; ++i) if (tm.grid().row_at_position(i) == 2) row = 2;
    CHECK(row == 2);
    tm.grid().sort_by(4, false);                // by CPU %, highest first
    CHECK(tm.grid().row_at_position(0) == 2);

    // The selection follows its thread when the list changes.
    tm.grid().select_row(2);
    mon.worker_ticks = 700;
    tm.update(3000000);
    CHECK(tm.grid().selected_row() == 2 && tm.thread_load_tenths(7) == 100);
    mon.second_thread = false;                  // the worker exits
    tm.update(4000000);
    CHECK(tm.grid().row_count() == 2 && tm.grid().selected_row() == -1 && tm.thread_load_tenths(7) == -1);
    w.render();
    mgr.remove_window(&tm);
  }
  CHECK(a.live == 0);
}

// --- Text editor ---------------------------------------------------------------------------------------------

static void type_text(Window& w, const char* text) {
  for (; *text; ++text) type_char(w, *text);
}
static bool text_is(TextEditor& e, const char* expected) { return e.text().equals(expected); }

static void test_text_editor() {
  TestAllocator a;
  {
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    TextEditor ed(a);
    ed.set_bounds({10, 10, 300, 100});
    w.add(&ed);
    w.layout();
    w.set_focus(&ed);
    Counter edits, moves;
    ed.on_changed(edits.handler());
    ed.on_caret_moved(moves.handler());
    CHECK(ed.line_count() == 1 && !ed.modified());

    type_text(w, "ab");
    press(w, key::kEnter);
    type_text(w, "  cd");
    press(w, key::kEnter);                      // keeps the indentation
    type_text(w, "ef");
    CHECK(text_is(ed, "ab\n  cd\n  ef") && ed.modified() && edits.count == 10);
    CHECK(ed.line_count() == 3 && ed.caret_line() == 2 && ed.caret_column() == 4 && ed.line_length(1) == 4);

    // Up and Down aim for the column they started from.
    press(w, key::kUp);
    CHECK(ed.caret_line() == 1 && ed.caret_column() == 4);
    press(w, key::kUp);
    CHECK(ed.caret_line() == 0 && ed.caret_column() == 2);
    press(w, key::kDown);
    CHECK(ed.caret_line() == 1 && ed.caret_column() == 4);
    press(w, 0x4A);                             // Home
    CHECK(ed.caret() == 3);
    press(w, 0x4D);                             // End
    CHECK(ed.caret() == 7);
    press(w, key::kLeft);
    press(w, key::kRight);
    CHECK(ed.caret() == 7);

    // Selecting and replacing.
    press(w, key::kLeft, modifier::kLeftShift);
    press(w, key::kLeft, modifier::kLeftShift);
    CHECK(ed.has_selection() && ed.selection_start() == 5 && ed.selection_end() == 7);
    type_char(w, 'X');
    CHECK(text_is(ed, "ab\n  X\n  ef") && !ed.has_selection());

    // Line breaks are deleted like any other character.
    press(w, 0x4A);
    press(w, key::kBackspace);                  // at the start of the line: it joins the previous one
    CHECK(ed.line_count() == 2 && text_is(ed, "ab  X\n  ef") && ed.caret() == 2);
    press(w, 0x4C);                             // Delete the character after the caret
    CHECK(text_is(ed, "ab X\n  ef"));

    // Clipboard: copy everything, paste it at the end.
    press(w, key::kA, modifier::kLeftControl);
    CHECK(ed.selection_start() == 0 && ed.selection_end() == ed.text().size());
    press(w, key::kA + 2, modifier::kLeftControl);
    press(w, 0x4D, modifier::kLeftControl);
    CHECK(ed.caret() == ed.text().size());
    press(w, key::kA + 21, modifier::kLeftControl);
    CHECK(text_is(ed, "ab X\n  efab X\n  ef"));
    press(w, 0x4A, modifier::kLeftControl);
    press(w, 0x4D, modifier::kLeftShift);       // select the first line
    press(w, key::kA + 23, modifier::kLeftControl);   // cut it
    CHECK(text_is(ed, "\n  efab X\n  ef") && ed.caret() == 0);
    press(w, key::kA + 18, modifier::kLeftControl);  // not the editor's: left for the parent
    CHECK(text_is(ed, "\n  efab X\n  ef"));

    // Tabs show as spaces up to the next multiple of four.
    ed.set_text("");
    CHECK(!ed.modified() && ed.line_count() == 1);
    press(w, key::kTab);
    type_char(w, 'x');
    CHECK(text_is(ed, "\tx") && ed.caret_column() == 5 && ed.offset_at(0, 1) == 0 && ed.offset_at(0, 2) == 1 && ed.column_at(0, 1) == 4);
    ed.set_read_only(true);
    type_char(w, 'y');
    CHECK(text_is(ed, "\tx"));
    ed.set_read_only(false);

    // Long text scrolls and keeps the caret in view.
    String lines(a);
    for (int i = 0; i < 40; ++i) {
      lines.append("line number ");
      lines.append_uint(static_cast<uint64_t>(i));
      lines.append(i == 5 ? " with a very long tail that does not fit in the editor width at all, not even close" : "");
      lines.append("\n");
    }
    ed.set_text(lines.view());
    w.layout();
    CHECK(ed.line_count() == 41 && ed.vertical_scroll_bar().visible() && ed.horizontal_scroll_bar().visible() && ed.top_line() == 0);
    press(w, 0x4D, modifier::kLeftControl);
    CHECK(ed.caret_line() == 40 && ed.top_line() > 0 && ed.caret_line() < ed.top_line() + ed.visible_lines());
    press(w, 0x4A, modifier::kLeftControl);
    CHECK(ed.top_line() == 0);
    for (int i = 0; i < 5; ++i) press(w, key::kDown);
    press(w, 0x4D);
    CHECK(ed.left_column() > 0 && ed.caret_column() > ed.visible_columns());
    press(w, 0x4A);
    CHECK(ed.left_column() == 0);
    press(w, 0x4E);                             // Page Down
    CHECK(ed.caret_line() == 5 + ed.visible_lines() - 1);
    w.mouse_wheel(60, 60, -1);
    CHECK(ed.top_line() >= 3);
    w.render();

    // The mouse places the caret and drags selections.
    ed.set_text("alpha\nbeta\ngamma");
    w.layout();
    const Point c = ed.cell_origin(1, 2);
    click(w, 10 + c.x, 10 + c.y + 4);
    CHECK(ed.caret() == 8 && !ed.has_selection());
    const Point d = ed.cell_origin(2, 3);
    w.mouse_move(10 + c.x, 10 + c.y + 4);
    w.mouse_button(10 + c.x, 10 + c.y + 4, MouseButton::Left, true);
    w.mouse_move(10 + d.x, 10 + d.y + 4);
    w.mouse_button(10 + d.x, 10 + d.y + 4, MouseButton::Left, false);
    CHECK(ed.selection_start() == 8 && ed.selection_end() == 14);
    w.render();
    ed.set_max_length(10);
    CHECK(!ed.insert_text("0123456789"));
    CHECK(!ed.set_text("this is longer than ten"));
  }
  CHECK(a.live == 0);
}

static void test_notepad() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    make_file(fs, "/t.txt", "a\r\nb\r\n");
    CHECK(fs.create_directory("/dir") == filesystem::Status::Ok);
    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::Notepad np(a);
    CHECK(mgr.add_window(&np));
    w.layout();
    CHECK(!np.open_file("/t.txt") && np.status_label().text().equals("No storage available."));
    np.set_file_system(&fs);
    CHECK(np.title().equals("Text Editor - Untitled") && np.position_label().text().equals("Ln 1, Col 1"));

    CHECK(np.open_file("t.txt"));               // the leading slash is optional; CRs are dropped
    CHECK(np.editor().text().equals("a\nb\n") && np.path().equals("/t.txt") && !np.editor().modified());
    CHECK(np.title().equals("Text Editor - t.txt") && np.status_label().text().equals("Opened /t.txt"));
    CHECK(!np.open_file("/nope") && np.status_label().text().equals("Cannot open /nope: not found") && np.editor().text().equals("a\nb\n"));
    CHECK(!np.open_file("/dir"));

    // Unsaved changes are only thrown away when the command is repeated.
    w.set_focus(&np.editor());
    np.editor().set_caret(np.editor().text().size());
    type_char(w, 'c');
    CHECK(np.editor().modified() && np.title().equals("Text Editor - t.txt *") && np.position_label().text().equals("Ln 3, Col 2"));
    CHECK(!np.new_document() && np.status_label().text().first(13).equals("Unsaved chang"));
    type_char(w, 'd');                          // typing again disarms it
    CHECK(!np.new_document() && np.editor().text().equals("a\nb\ncd"));
    CHECK(np.new_document() && np.editor().text().empty() && np.path().empty() && np.title().equals("Text Editor - Untitled"));

    // Saving writes the text back (Ctrl+S, to the path in the toolbar).
    np.path_box().set_text("out.txt");
    type_text(w, "hello");
    press(w, key::kEnter);
    type_text(w, "world");
    press(w, key::kA + 18, modifier::kLeftControl);
    CHECK(!np.editor().modified() && np.path().equals("/out.txt") && np.status_label().text().equals("Saved /out.txt") && np.title().equals("Text Editor - out.txt"));
    UniquePointer<filesystem::IFile> f;
    CHECK(fs.open_file("/out.txt", filesystem::OpenMode::Read, &f) == filesystem::Status::Ok && f->size() == 11);
    char got[16] = {};
    uint64_t n = 0;
    CHECK(f->read(0, ByteView(reinterpret_cast<uint8_t*>(got), 11), &n) == filesystem::Status::Ok && !strcmp(got, "hello\nworld"));
    f.reset();
    CHECK(!np.save_file("/missing/x.txt") && np.status_label().text().equals("Cannot save /missing/x.txt: not found"));
    CHECK(!np.save_file("") == false);          // the toolbar's path again
    np.path_box().set_text("");
    CHECK(!np.save_file() && np.status_label().text().equals("Enter a path to save to."));

    // Enter in the path box opens the file.
    np.path_box().set_text("/t.txt");
    w.set_focus(&np.path_box());
    press(w, key::kEnter);
    CHECK(np.editor().text().equals("a\nb\n"));
    w.render();
    mgr.remove_window(&np);
  }
  CHECK(a.live == 0);
}

// --- Breadcrumb bar ------------------------------------------------------------------------------------------

static void test_breadcrumb_bar() {
  TestAllocator a;
  {
    Screen screen(a, 400, 200);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 200});
    BreadcrumbBar bar(a);
    TextBox other(a);
    bar.set_bounds({10, 10, 300, 24});
    other.set_bounds({10, 100, 100, 24});
    w.add(&bar);
    w.add(&other);
    w.layout();
    Counter went;
    bar.on_navigate(went.handler());
    CHECK(bar.path().equals("/") && bar.segment_count() == 1 && bar.segment_label(0).equals("/"));

    bar.set_path("docs//old/");
    w.layout();
    CHECK(bar.path().equals("/docs/old") && bar.segment_count() == 3);
    CHECK(bar.segment_label(0).equals("/") && bar.segment_label(1).equals("docs") && bar.segment_label(2).equals("old"));
    CHECK(bar.segment(0).path_length == 0 && bar.segment(1).path_length == 5 && bar.segment(2).path_length == 9);
    CHECK(bar.segment(1).x > bar.segment(0).x + bar.segment(0).width && bar.segment(2).x > bar.segment(1).x + bar.segment(1).width);

    // Hovering highlights a folder, clicking one reports where it goes.
    auto center = [&](uint32_t i) { return Point{10 + bar.segment(i).x + bar.segment(i).width / 2, 10 + 12}; };
    w.mouse_move(center(1).x, center(1).y);
    CHECK(bar.hovered_segment() == 1);
    w.mouse_move(300, 150);
    CHECK(bar.hovered_segment() == -1);
    click(w, center(1).x, center(1).y);
    CHECK(went.count == 1 && bar.requested_path().equals("/docs") && bar.path().equals("/docs/old"));   // the program sets the path
    click(w, center(0).x, center(0).y);
    CHECK(went.count == 2 && bar.requested_path().equals("/"));
    click(w, center(2).x, center(2).y);
    CHECK(went.count == 3 && bar.requested_path().equals("/docs/old"));
    w.render();

    // Clicking beside the last folder turns the bar into a text box.
    click(w, 10 + 290, 10 + 12);
    CHECK(bar.editing() && bar.edit_box().visible() && w.focus() == &bar.edit_box() && bar.edit_box().text().equals("/docs/old"));
    w.render();
    bar.edit_box().set_text("  a//b/ ");
    press(w, key::kEnter);
    CHECK(!bar.editing() && !bar.edit_box().visible() && went.count == 4 && bar.requested_path().equals("/a/b"));
    bar.edit_box().set_text("");
    bar.begin_edit();
    bar.edit_box().set_text("");
    press(w, key::kEnter);
    CHECK(went.count == 5 && bar.requested_path().equals("/"));

    // Escape and moving the focus elsewhere cancel.
    bar.begin_edit();
    press(w, key::kEscape);
    CHECK(!bar.editing() && went.count == 5);
    bar.begin_edit();
    w.set_focus(&other);
    CHECK(!bar.editing() && went.count == 5);

    // A path too long for the bar gives its first folders up for an ellipsis that goes to the one before the first shown.
    bar.set_bounds({10, 10, 150, 24});
    bar.set_path("/aaaaaaaa/bbbbbbbb/cccccccc/dddddddd");
    w.layout();
    CHECK(bar.segment_count() == 2 && bar.segment_label(0).equals("...") && bar.segment_label(1).equals("dddddddd"));
    click(w, 10 + bar.segment(0).x + 4, 10 + 12);
    CHECK(bar.requested_path().equals("/aaaaaaaa/bbbbbbbb/cccccccc"));
    click(w, 10 + bar.segment(1).x + 4, 10 + 12);
    CHECK(bar.requested_path().equals("/aaaaaaaa/bbbbbbbb/cccccccc/dddddddd"));
    bar.set_bounds({10, 10, 60, 24});
    w.layout();
    CHECK(bar.segment_count() >= 1 && bar.segment_label(bar.segment_count() - 1).equals("dddddddd"));
    w.render();
    bar.set_bounds({10, 10, 390, 24});
    w.layout();
    CHECK(bar.segment_count() == 5 && bar.segment_label(0).equals("/"));
  }
  CHECK(a.live == 0);
}

// --- Tab control ---------------------------------------------------------------------------------------------

static void test_tab_control() {
  TestAllocator a;
  {
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    TabControl tabs(a);
    Panel one(a), two(a), three(a);
    Button inside(a);
    inside.set_dock(DockStyle::Top);
    inside.set_size(0, 30);
    one.add(&inside);
    tabs.set_bounds({10, 10, 300, 200});
    w.add(&tabs);
    Counter changes;
    tabs.on_selection_changed(changes.handler());
    CHECK(tabs.selected_page() == -1 && tabs.add_page("One", &one) == 0 && tabs.add_page("Second", &two) == 1 && tabs.add_page("3", &three) == 2);
    CHECK(tabs.add_page("none", nullptr) == -1 && tabs.page_count() == 3 && tabs.selected_page() == 0);
    w.layout();
    CHECK(one.visible() && !two.visible() && !three.visible() && changes.count == 0);
    const Rect2D area = tabs.page_rect();
    CHECK(one.bounds().x == area.x && one.bounds().y == area.y && one.bounds().width == area.width && one.bounds().height == area.height);
    CHECK(area.y >= TabControl::kTabHeight && area.y + static_cast<int32_t>(area.height) <= 200 && inside.bounds().width == area.width);

    // Tabs sit side by side, the selected one a little larger; a click selects.
    const Rect2D t0 = tabs.tab_rect(0), t1 = tabs.tab_rect(1), t2 = tabs.tab_rect(2);
    CHECK(t1.x >= t0.x + static_cast<int32_t>(t0.width) - 4 && t2.x >= t1.x + static_cast<int32_t>(t1.width) && t0.height > t1.height);
    CHECK(tabs.tab_at(t1.x + 3, t1.y + 5) == 1 && tabs.tab_at(250, 10) == -1 && tabs.tab_at(t0.x + 3, 100) == -1);
    click(w, 10 + t1.x + t1.width / 2, 10 + 12);
    CHECK(tabs.selected_page() == 1 && !one.visible() && two.visible() && changes.count == 1 && w.focus() == &tabs);
    w.render();
    click(w, 10 + tabs.tab_rect(1).x + 8, 10 + 12);   // the selected tab again: nothing changes
    CHECK(changes.count == 1);

    // Arrow keys with the focus on the tabs, Ctrl+Tab from inside a page.
    press(w, key::kRight);
    CHECK(tabs.selected_page() == 2 && three.visible());
    press(w, key::kRight);
    CHECK(tabs.selected_page() == 0);          // wraps
    press(w, key::kLeft);
    CHECK(tabs.selected_page() == 2);
    tabs.select_page(0);
    w.set_focus(&inside);
    press(w, key::kTab, modifier::kLeftControl);
    CHECK(tabs.selected_page() == 1);
    press(w, key::kTab, modifier::kLeftControl | modifier::kLeftShift);
    CHECK(tabs.selected_page() == 0);
    tabs.select_page(7);
    tabs.select_page(-1);
    CHECK(tabs.selected_page() == 0);
    w.render();
  }
  CHECK(a.live == 0);
}

// --- Tool strip, color palette, drawing canvas and Paint -------------------------------------------------------

static void test_tool_strip() {
  TestAllocator a;
  {
    Screen screen(a, 300, 200);
    Theme theme;
    Window w(a, screen.renderer, theme, {300, 200});
    ToolStrip strip(a);
    static int drawn = 0;
    ToolIcon icon = [](Painter& p, const Rect2D& r, uint32_t ink) { ++drawn; p.fill(r, ink); };
    strip.set_columns(2);
    const char* const tool_names[5] = {"A", "B", "C", "D", "E"};
    for (const char* name : tool_names) strip.add_tool(name, icon);
    CHECK(strip.tool_count() == 5 && strip.selected_tool() == 0 && strip.tool_name(2).equals("C"));
    const Size size = strip.content_size();
    CHECK(size.width == 2 * ToolStrip::kPad + 2 * ToolStrip::kButton && size.height == 2 * ToolStrip::kPad + 3 * ToolStrip::kButton);
    strip.set_bounds({10, 10, static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height)});
    w.add(&strip);
    w.layout();
    Counter selected, hovered;
    strip.on_selection_changed(selected.handler());
    strip.on_hover_changed(hovered.handler());
    const Rect2D r3 = strip.tool_rect(3), r0 = strip.tool_rect(0);
    CHECK(r3.x == r0.x + ToolStrip::kButton && r3.y == r0.y + ToolStrip::kButton && strip.tool_at(r3.x + 2, r3.y + 2) == 3 && strip.tool_at(0, 0) == -1);
    w.mouse_move(10 + r3.x + 5, 10 + r3.y + 5);
    CHECK(strip.hovered_tool() == 3 && hovered.count == 1);
    w.mouse_move(250, 150);
    CHECK(strip.hovered_tool() == -1 && hovered.count == 2);
    click(w, 10 + r3.x + 5, 10 + r3.y + 5);
    CHECK(strip.selected_tool() == 3 && selected.count == 1 && w.focus() == &strip);
    press(w, key::kUp);
    CHECK(strip.selected_tool() == 1);
    press(w, key::kRight);
    CHECK(strip.selected_tool() == 2);
    press(w, key::kDown);
    press(w, key::kDown);                       // past the end: stays
    CHECK(strip.selected_tool() == 4);
    strip.select_tool(9);
    CHECK(strip.selected_tool() == 4 && selected.count == 4);
    w.render();
    CHECK(drawn >= 5);
  }
  CHECK(a.live == 0);
}

static void test_color_palette() {
  TestAllocator a;
  {
    Screen screen(a, 400, 100);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 100});
    ColorPalette pal(a);
    pal.set_bounds({10, 10, 360, 50});
    w.add(&pal);
    w.layout();
    Counter changes;
    pal.on_colors_changed(changes.handler());
    CHECK(pal.color_count() == 28 && pal.columns() == 14 && pal.primary() == 0xFF000000 && pal.secondary() == 0xFFFFFFFF);
    const Rect2D s16 = pal.swatch_rect(16), s2 = pal.swatch_rect(2);
    CHECK(pal.color_at(16) == 0xFFFF0000 && s16.x == s2.x && s16.y > s2.y && pal.swatch_at(s16.x + 3, s16.y + 3) == 16);
    click(w, 10 + s16.x + 3, 10 + s16.y + 3);
    CHECK(pal.primary() == 0xFFFF0000 && changes.count == 1);
    const Rect2D s18 = pal.swatch_rect(18);
    w.mouse_move(10 + s18.x + 3, 10 + s18.y + 3);
    w.mouse_button(10 + s18.x + 3, 10 + s18.y + 3, MouseButton::Right, true);
    w.mouse_button(10 + s18.x + 3, 10 + s18.y + 3, MouseButton::Right, false);
    CHECK(pal.secondary() == 0xFF00FF00 && changes.count == 2);
    click(w, 10 + pal.indicator_rect().x + 30, 10 + pal.indicator_rect().y + 30);   // the squares: swap
    CHECK(pal.primary() == 0xFF00FF00 && pal.secondary() == 0xFFFF0000 && changes.count == 3);
    pal.set_primary(0x00123456);
    CHECK(pal.primary() == 0xFF123456);
    pal.set_primary(0xFF123456);
    CHECK(changes.count == 4);
    const uint32_t mine[3] = {0x00FF0000, 0x0000FF00, 0x000000FF};
    CHECK(pal.set_colors(mine, 3) && pal.color_count() == 3 && pal.color_at(0) == 0xFFFF0000 && pal.columns() == 2);
    w.render();
  }
  CHECK(a.live == 0);
}

static void drag(Window& w, Point from, Point to, MouseButton button = MouseButton::Left) {
  w.mouse_move(from.x, from.y);
  w.mouse_button(from.x, from.y, button, true);
  w.mouse_move((from.x + to.x) / 2, (from.y + to.y) / 2);
  w.mouse_move(to.x, to.y);
  w.mouse_button(to.x, to.y, button, false);
}

// Window coordinates of the middle of bitmap pixel (x, y).
static Point pixel_point(DrawingCanvas& c, int32_t x, int32_t y) {
  const Point v = c.image_to_view(x, y), o = c.window_origin();
  return {o.x + v.x + static_cast<int32_t>(c.zoom()) / 2, o.y + v.y + static_cast<int32_t>(c.zoom()) / 2};
}

static void test_drawing_canvas() {
  TestAllocator a;
  {
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    DrawingCanvas c(a);
    c.set_bounds({10, 10, 300, 220});
    w.add(&c);
    CHECK(!c.new_image(0, 10) && !c.new_image(5000, 10) && c.image_width() == 0);
    CHECK(c.new_image(100, 80) && c.image_width() == 100 && c.image_height() == 80 && c.pixel(5, 5) == 0xFFFFFFFF && c.pixel(100, 5) == 0);
    w.layout();
    Counter changes, picks;
    c.on_changed(changes.handler());
    c.on_color_picked(picks.handler());
    const int32_t base = changes.count;
    const uint32_t black = 0xFF000000, white = 0xFFFFFFFF, red = 0xFFFF0000, blue = 0xFF0000FF;

    // The pencil follows the pointer; the left button paints the primary color, the right button the secondary.
    c.set_primary(black);
    c.set_secondary(red);
    drag(w, pixel_point(c, 5, 5), pixel_point(c, 20, 5));
    bool row_ok = true;
    for (uint32_t x = 5; x <= 20; ++x) row_ok = row_ok && c.pixel(x, 5) == black;
    CHECK(row_ok && c.pixel(4, 5) == white && c.pixel(21, 5) == white && c.pixel(10, 6) == white);
    CHECK(c.modified() && c.can_undo() && !c.can_redo() && changes.count == base + 1 && !c.stroke_active());
    drag(w, pixel_point(c, 5, 8), pixel_point(c, 10, 8), MouseButton::Right);
    CHECK(c.pixel(7, 8) == red);
    c.undo();
    CHECK(c.pixel(7, 8) == white && c.pixel(10, 5) == black && c.can_redo());
    c.undo();
    CHECK(c.pixel(10, 5) == white && !c.can_undo());
    c.redo();
    CHECK(c.pixel(10, 5) == black && c.pixel(7, 8) == white);
    w.set_focus(&c);
    press(w, key::kA + 24, modifier::kLeftControl);   // Ctrl+Y
    CHECK(c.pixel(7, 8) == red);
    press(w, key::kA + 25, modifier::kLeftControl);   // Ctrl+Z
    CHECK(c.pixel(7, 8) == white);

    // A wider brush paints a disc.
    c.set_brush_size(5);
    drag(w, pixel_point(c, 50, 50), pixel_point(c, 50, 50));
    CHECK(c.pixel(50, 50) == black && c.pixel(52, 50) == black && c.pixel(50, 48) == black && c.pixel(53, 50) == white && c.pixel(52, 52) == white);
    c.set_brush_size(1);

    // The eraser paints the secondary color.
    c.set_tool(PaintTool::Eraser);
    drag(w, pixel_point(c, 49, 50), pixel_point(c, 51, 50));
    CHECK(c.pixel(50, 50) == red && c.pixel(50, 51) == red);

    // Shapes are previewed while dragging and drawn when the button goes up.
    c.set_tool(PaintTool::Rectangle);
    w.mouse_move(pixel_point(c, 10, 20).x, pixel_point(c, 10, 20).y);
    w.mouse_button(pixel_point(c, 10, 20).x, pixel_point(c, 10, 20).y, MouseButton::Left, true);
    w.mouse_move(pixel_point(c, 30, 40).x, pixel_point(c, 30, 40).y);
    CHECK(c.stroke_active() && c.pixel(30, 30) == black && c.pixel(20, 30) == white);
    w.mouse_move(pixel_point(c, 25, 35).x, pixel_point(c, 25, 35).y);   // dragging back redraws it smaller
    CHECK(c.pixel(30, 30) == white && c.pixel(25, 30) == black);
    w.mouse_button(pixel_point(c, 25, 35).x, pixel_point(c, 25, 35).y, MouseButton::Left, false);
    CHECK(!c.stroke_active() && c.pixel(10, 20) == black && c.pixel(25, 35) == black && c.pixel(15, 20) == black && c.pixel(15, 28) == white);
    c.set_filled(true);
    drag(w, pixel_point(c, 40, 20), pixel_point(c, 60, 30));
    CHECK(c.pixel(40, 20) == black && c.pixel(60, 30) == black && c.pixel(50, 25) == red);

    c.set_tool(PaintTool::Ellipse);
    c.set_filled(false);
    drag(w, pixel_point(c, 10, 50), pixel_point(c, 30, 70));
    CHECK(c.pixel(10, 60) == black && c.pixel(30, 60) == black && c.pixel(20, 50) == black && c.pixel(20, 70) == black && c.pixel(20, 60) == white && c.pixel(10, 50) == white);
    c.set_filled(true);
    drag(w, pixel_point(c, 60, 50), pixel_point(c, 80, 70));
    CHECK(c.pixel(70, 60) == red && c.pixel(60, 60) == black && c.pixel(60, 50) == white);
    c.set_filled(false);

    c.set_tool(PaintTool::Line);
    drag(w, pixel_point(c, 0, 75), pixel_point(c, 20, 79));
    CHECK(c.pixel(0, 75) == black && c.pixel(20, 79) == black && c.pixel(10, 77) == black);

    // Fill stays inside the outline it is started in.
    c.set_tool(PaintTool::Fill);
    c.set_primary(blue);
    click(w, pixel_point(c, 15, 25).x, pixel_point(c, 15, 25).y);
    CHECK(c.pixel(15, 25) == blue && c.pixel(24, 34) == blue && c.pixel(10, 20) == black && c.pixel(9, 25) == white && c.pixel(26, 36) == white);
    click(w, pixel_point(c, 90, 10).x, pixel_point(c, 90, 10).y);   // the outside: everything connected to it
    CHECK(c.pixel(95, 75) == blue && c.pixel(9, 25) == blue && c.pixel(15, 25) == blue);
    c.undo();
    CHECK(c.pixel(95, 75) == white);

    // The color picker takes the color under the pointer (left: primary, right: secondary).
    c.set_tool(PaintTool::Picker);
    const int32_t history = static_cast<int32_t>(c.can_undo());
    click(w, pixel_point(c, 10, 20).x, pixel_point(c, 10, 20).y);
    CHECK(c.primary() == black && picks.count == 1 && history == 1);
    drag(w, pixel_point(c, 15, 25), pixel_point(c, 15, 25), MouseButton::Right);
    CHECK(c.secondary() == blue && picks.count == 2);

    // Escape cancels a stroke.
    c.set_tool(PaintTool::Line);
    w.set_focus(&c);
    const uint32_t before = c.pixel(80, 10);
    w.mouse_move(pixel_point(c, 70, 5).x, pixel_point(c, 70, 5).y);
    w.mouse_button(pixel_point(c, 70, 5).x, pixel_point(c, 70, 5).y, MouseButton::Left, true);
    w.mouse_move(pixel_point(c, 90, 15).x, pixel_point(c, 90, 15).y);
    CHECK(c.stroke_active() && c.pixel(80, 10) == black);
    press(w, key::kEscape);
    CHECK(!c.stroke_active() && c.pixel(80, 10) == before);
    w.mouse_button(pixel_point(c, 90, 15).x, pixel_point(c, 90, 15).y, MouseButton::Left, false);
    CHECK(c.pixel(80, 10) == before);

    // Zoom: pixels grow, scroll bars appear, and the pointer still hits the pixel it points at.
    c.set_tool(PaintTool::Pencil);
    c.set_zoom(4);
    w.layout();
    c.horizontal_scroll_bar().set_value(0);
    c.vertical_scroll_bar().set_value(0);
    CHECK(c.zoom() == 4 && c.horizontal_scroll_bar().visible() && c.vertical_scroll_bar().visible());
    drag(w, pixel_point(c, 2, 2), pixel_point(c, 4, 2));
    CHECK(c.pixel(2, 2) == black && c.pixel(4, 2) == black && c.pixel(5, 2) == white);
    int32_t ix, iy;
    CHECK(c.view_to_image(c.image_to_view(3, 3).x + 3, c.image_to_view(3, 3).y + 3, &ix, &iy) && ix == 3 && iy == 3);
    c.set_grid(true);
    c.set_zoom(8);
    CHECK(c.zoom() == 8);
    w.render();
    c.zoom_in();
    c.zoom_in();
    c.zoom_in();
    CHECK(c.zoom() == 16);
    c.zoom_in();
    CHECK(c.zoom() == 16);
    w.mouse_wheel(60, 60, -1);
    CHECK(c.scroll_position().y > 0);
    w.render();
    c.set_zoom(1);
    c.set_grid(false);
    w.layout();
    CHECK(c.zoom() == 1 && !c.horizontal_scroll_bar().visible());
    int32_t px, py;
    w.mouse_move(pixel_point(c, 33, 44).x, pixel_point(c, 33, 44).y);
    CHECK(c.pointer_position(&px, &py) && px == 33 && py == 44);
    w.mouse_move(390, 290);
    CHECK(!c.pointer_position(&px, &py));

    // History is limited; the oldest strokes are forgotten.
    c.set_tool(PaintTool::Pencil);
    for (int i = 0; i < 70; ++i) drag(w, pixel_point(c, 1, 1), pixel_point(c, 2 + i % 3, 1));
    uint32_t undone = 0;
    while (c.can_undo() && undone < 200) { c.undo(); ++undone; }
    CHECK(undone == DrawingCanvas::kMaxHistoryEntries);

    // Pixels with transparency are laid over white.
    const uint32_t src[4] = {0x80000000, 0xFFFF0000, 0x00000000, 0xFF00FF00};
    CHECK(c.set_pixels(src, 2, 2) && c.image_width() == 2 && !c.modified() && !c.can_undo());
    CHECK(c.pixel(1, 0) == 0xFFFF0000 && c.pixel(0, 1) == 0xFFFFFFFF && c.pixel(1, 1) == 0xFF00FF00);
    CHECK(red_of(c.pixel(0, 0)) >= 0x7F && red_of(c.pixel(0, 0)) <= 0x80 && green_of(c.pixel(0, 0)) == red_of(c.pixel(0, 0)));
    CHECK(c.surface().width == 2 && c.surface().valid());
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_paint() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    make_file(fs, "/notes.txt", "not a picture");
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::Paint paint(a);
    CHECK(mgr.add_window(&paint));
    w.layout();
    DrawingCanvas& c = paint.canvas();
    CHECK(desktop::Paint::is_picture_name("a/b.PNG") && desktop::Paint::is_picture_name("x.jpeg") && !desktop::Paint::is_picture_name("x.txt") && !desktop::Paint::is_picture_name("png"));
    CHECK(paint.title().equals("Paint - Untitled") && c.image_width() == 640 && c.image_height() == 480 && !paint.menu_bar().find_item("Edit", "Undo")->enabled());
    CHECK(paint.tools().tool_count() == 8 && paint.tools().tool_name(3).equals("Rectangle") && paint.tools().tool_name(7).equals("Select"));

    // The tool strip, palette, size slider and options drive the canvas.
    paint.tools().select_tool(3);
    CHECK(c.tool() == PaintTool::Rectangle);
    paint.tools().select_tool(0);
    CHECK(c.tool() == PaintTool::Pencil);
    const Rect2D swatch = paint.palette().swatch_rect(16);
    const Point po = paint.palette().window_origin();
    click(w, po.x + swatch.x + 3, po.y + swatch.y + 3);
    CHECK(paint.palette().primary() == 0xFFFF0000 && c.primary() == 0xFFFF0000 && paint.color_picker().color() == 0xFFFF0000);
    paint.size_slider().set_value(3);
    CHECK(c.brush_size() == 3);
    CHECK(choose(w, paint.menu_bar(), "Options", "Filled Shapes") && choose(w, paint.menu_bar(), "View", "Pixel Grid"));
    CHECK(c.filled() && c.grid() && paint.menu_bar().find_item("Options", "Filled Shapes")->checked() && paint.menu_bar().find_item("View", "Pixel Grid")->checked());
    CHECK(choose(w, paint.menu_bar(), "Options", "Filled Shapes") && choose(w, paint.menu_bar(), "View", "Pixel Grid"));
    CHECK(!c.filled() && !c.grid() && !paint.menu_bar().find_item("Options", "Filled Shapes")->checked());
    CHECK(choose(w, paint.menu_bar(), "View", "Zoom In"));
    CHECK(c.zoom() == 2 && paint.zoom_label().text().equals("200%"));
    CHECK(choose(w, paint.menu_bar(), "View", "Zoom Out"));
    CHECK(c.zoom() == 1 && paint.zoom_label().text().equals("100%"));

    // Drawing marks the picture modified and enables Undo.
    w.layout();
    drag(w, pixel_point(c, 10, 10), pixel_point(c, 40, 10));
    CHECK(c.pixel(25, 10) == 0xFFFF0000 && c.modified() && paint.menu_bar().find_item("Edit", "Undo")->enabled() && !paint.menu_bar().find_item("Edit", "Redo")->enabled());
    CHECK(paint.title().equals("Paint - Untitled *"));
    CHECK(choose(w, paint.menu_bar(), "Edit", "Undo"));
    CHECK(c.pixel(25, 10) == 0xFFFFFFFF && paint.menu_bar().find_item("Edit", "Redo")->enabled());
    CHECK(choose(w, paint.menu_bar(), "Edit", "Redo"));
    CHECK(c.pixel(25, 10) == 0xFFFF0000);

    // The color picker tool feeds the palette.
    paint.tools().select_tool(6);
    click(w, pixel_point(c, 25, 10).x, pixel_point(c, 25, 10).y);
    CHECK(paint.palette().primary() == 0xFFFF0000);
    paint.palette().set_primary(0xFF0000FF);
    CHECK(c.primary() == 0xFF0000FF);

    // Saving and opening: the format follows the extension; PNG is the default.
    CHECK(!paint.save_file("/x.png") && paint.status_label().text().equals("No storage available."));
    paint.set_file_system(&fs);
    CHECK(!paint.save_file("") && paint.status_label().text().equals("Enter a path to save to."));
    CHECK(!paint.save_file("/dir/missing.png"));
    CHECK(paint.save_file("art"));
    CHECK(paint.path().equals("/art.png") && !c.modified() && paint.title().equals("Paint - art.png") && paint.status_label().text().equals("Saved /art.png"));
    UniquePointer<filesystem::IFile> f;
    CHECK(fs.open_file("/art.png", filesystem::OpenMode::Read, &f) == filesystem::Status::Ok && f->size() > 8);
    uint8_t magic[4] = {};
    uint64_t n = 0;
    CHECK(f->read(0, ByteView(magic, 4), &n) == filesystem::Status::Ok && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G');
    f.reset();
    const char* names[] = {"/art.bmp", "/art.qoi", "/art.tga", "/art.ppm"};
    for (const char* name : names) {
      CHECK(paint.save_file(name));
      c.new_image(8, 8, 0xFF123456);             // blank, then bring the picture back
      CHECK(paint.open_file(name) && c.image_width() == 640 && c.pixel(25, 10) == 0xFFFF0000 && c.pixel(25, 11) == 0xFFFF0000 && c.pixel(100, 100) == 0xFFFFFFFF);
    }
    CHECK(paint.open_file("/art.png") && c.pixel(25, 10) == 0xFFFF0000 && !c.modified() && paint.title().equals("Paint - art.png"));
    CHECK(!paint.open_file("/missing.png") && paint.status_label().text().equals("Cannot open /missing.png: not found"));
    CHECK(!paint.open_file("/notes.txt") && paint.status_label().text().equals("That is not a picture this program can read."));

    // Unsaved changes survive the first New / Open.
    paint.tools().select_tool(0);
    drag(w, pixel_point(c, 60, 60), pixel_point(c, 70, 60));
    CHECK(c.modified());
    CHECK(!paint.new_picture() && paint.status_label().text().first(13).equals("Unsaved chang") && c.pixel(65, 60) == 0xFF0000FF);
    CHECK(paint.new_picture(200, 100) && c.image_width() == 200 && c.image_height() == 100 && c.pixel(65, 60) == 0xFFFFFFFF && paint.path().empty());
    w.mouse_move(799, 599);
    CHECK(paint.position_label().text().equals("200 x 100"));
    press(w, key::kA + 18, modifier::kLeftControl);   // Ctrl+S with no path
    CHECK(paint.status_label().text().equals("Enter a path to save to."));
    w.render();
    mgr.remove_window(&paint);
  }
  CHECK(a.live == 0);
}

// --- Clipboard -----------------------------------------------------------------------------------------------------

static void test_text_clipboard() {
  TestAllocator a;
  {
    kernel::Clipboard cb(a);
    Screen screen(a, 400, 200);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 200});
    w.set_clipboard(&cb);
    TextBox one(a), two(a);
    TextEditor ed(a);
    one.set_bounds({10, 10, 200, 24});
    two.set_bounds({10, 40, 200, 24});
    ed.set_bounds({10, 70, 300, 100});
    w.add(&one);
    w.add(&two);
    w.add(&ed);
    w.layout();
    String got(a);

    // Selecting in a text box: Shift with the arrows, Ctrl+A, replacing by typing.
    one.set_text("hello world");
    w.set_focus(&one);
    CHECK(!one.has_selection());
    for (int i = 0; i < 5; ++i) press(w, key::kLeft, modifier::kLeftShift);
    CHECK(one.has_selection() && one.selected_text().equals("world") && one.selection_start() == 6 && one.selection_end() == 11);
    press(w, key::kLeft);                       // collapses to the start of the selection
    CHECK(!one.has_selection() && one.caret() == 6);
    press(w, 0x4D, modifier::kLeftShift);       // Shift+End
    CHECK(one.selected_text().equals("world"));
    press(w, key::kA + 2, modifier::kLeftControl);   // copy
    CHECK(cb.get_text(&got) && got.view().equals("world") && one.text().equals("hello world"));
    press(w, key::kA + 23, modifier::kLeftControl);  // cut
    CHECK(one.text().equals("hello ") && cb.get_text(&got) && got.view().equals("world") && !one.has_selection());
    w.set_focus(&two);
    press(w, key::kA + 21, modifier::kLeftControl);  // paste into another control
    CHECK(two.text().equals("world") && two.caret() == 5);
    press(w, key::kA, modifier::kLeftControl);
    CHECK(two.selected_text().equals("world"));
    type_char(w, 'z');
    CHECK(two.text().equals("z"));
    press(w, key::kBackspace);
    CHECK(two.text().empty());

    // A drag with the mouse selects; Backspace removes the selection.
    one.set_text("abcdefgh");
    w.mouse_move(10 + 4 + 2 * 8, 22);
    w.mouse_button(10 + 4 + 2 * 8, 22, MouseButton::Left, true);
    w.mouse_move(10 + 4 + 6 * 8, 22);
    w.mouse_button(10 + 4 + 6 * 8, 22, MouseButton::Left, false);
    CHECK(one.selected_text().equals("cdef") && w.focus() == &one);
    press(w, key::kBackspace);
    CHECK(one.text().equals("abgh") && one.caret() == 2);

    // Line breaks become spaces in a single-line box; the maximum length holds.
    CHECK(cb.set_text("a\r\nb\tc"));
    two.set_text("");
    w.set_focus(&two);
    press(w, key::kA + 21, modifier::kLeftControl);
    CHECK(two.text().equals("a  b c"));
    two.set_text("");
    two.set_max_length(5);
    CHECK(cb.set_text("123456789"));
    press(w, key::kA + 21, modifier::kLeftControl);
    CHECK(two.text().equals("12345"));
    two.set_max_length(0);
    two.set_read_only(true);
    press(w, key::kA + 21, modifier::kLeftControl);   // read-only: nothing is pasted
    CHECK(two.text().equals("12345"));
    two.set_read_only(false);

    // Shift+Insert, Ctrl+Insert and Shift+Delete are the older keys for paste, copy and cut.
    two.set_text("");
    CHECK(cb.set_text("zz"));
    press(w, 0x49, modifier::kLeftShift);
    CHECK(two.text().equals("zz"));
    press(w, key::kA, modifier::kLeftControl);
    press(w, 0x49, modifier::kLeftControl);
    press(w, 0x4C, modifier::kLeftShift);
    CHECK(two.text().empty() && cb.get_text(&got) && got.view().equals("zz"));

    // A picture on the clipboard is not text.
    const uint32_t px[1] = {0xFF112233};
    CHECK(cb.set_image(px, 1, 1));
    press(w, key::kA + 21, modifier::kLeftControl);
    CHECK(two.text().empty());

    // The multi-line editor shares the clipboard; its line breaks are normalised.
    CHECK(cb.set_text("line1\r\nline2\rline3"));
    w.set_focus(&ed);
    press(w, key::kA + 21, modifier::kLeftControl);
    CHECK(ed.text().equals("line1\nline2\nline3") && ed.line_count() == 3);
    press(w, key::kA, modifier::kLeftControl);
    press(w, key::kA + 2, modifier::kLeftControl);
    CHECK(cb.get_text(&got) && got.view().equals("line1\nline2\nline3"));
    ed.set_caret(0);
    press(w, 0x4D, modifier::kLeftShift);
    press(w, key::kA + 23, modifier::kLeftControl);
    CHECK(cb.get_text(&got) && got.view().equals("line1") && ed.text().equals("\nline2\nline3"));
    w.set_focus(&one);
    press(w, key::kA + 21, modifier::kLeftControl);
    CHECK(one.text().equals("abline1gh"));
    w.render();

    // Without a clipboard in the window each control keeps a copy of its own.
    w.set_clipboard(nullptr);
    one.set_text("private");
    press(w, key::kA, modifier::kLeftControl);
    press(w, key::kA + 2, modifier::kLeftControl);
    press(w, 0x4D);
    press(w, key::kA + 21, modifier::kLeftControl);
    CHECK(one.text().equals("privateprivate"));
    cb.clear();
  }
  CHECK(a.live == 0);
}

static void test_canvas_clipboard() {
  TestAllocator a;
  {
    kernel::Clipboard cb(a);
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    DrawingCanvas c(a);
    c.set_bounds({10, 10, 300, 220});
    w.add(&c);
    c.new_image(40, 30);
    w.layout();
    w.set_focus(&c);
    const uint32_t white = 0xFFFFFFFF, red = 0xFFFF0000, blue = 0xFF0000FF;
    Counter changes, tools;
    c.on_changed(changes.handler());
    c.on_tool_changed(tools.handler());

    // Without a clipboard in the window nothing is copied or pasted.
    CHECK(!c.copy_selection() && !c.paste() && !c.cut_selection());
    w.set_clipboard(&cb);

    // A filled red square at (5,5)-(14,14).
    c.set_tool(PaintTool::Rectangle);
    c.set_filled(true);
    c.set_primary(red);
    c.set_secondary(red);
    drag(w, pixel_point(c, 5, 5), pixel_point(c, 14, 14));
    c.set_filled(false);
    c.set_secondary(white);
    CHECK(c.pixel(5, 5) == red && c.pixel(14, 14) == red && c.pixel(10, 10) == red && c.pixel(15, 15) == white);

    // The Select tool drags out a rectangle; a click clears it; Ctrl+A selects all.
    c.set_tool(PaintTool::Select);
    drag(w, pixel_point(c, 3, 3), pixel_point(c, 16, 16));
    CHECK(c.has_selection() && c.selection().x == 3 && c.selection().y == 3 && c.selection().width == 14 && c.selection().height == 14);
    click(w, pixel_point(c, 30, 25).x, pixel_point(c, 30, 25).y);
    CHECK(!c.has_selection());
    press(w, key::kA, modifier::kLeftControl);
    CHECK(c.has_selection() && c.selection().width == 40 && c.selection().height == 30);
    w.render();
    press(w, key::kEscape);
    CHECK(!c.has_selection());
    drag(w, pixel_point(c, 5, 5), pixel_point(c, 14, 14));
    CHECK(c.selection().width == 10 && c.selection().height == 10);

    // Copy takes the selection (the whole picture without one); the picture is unchanged.
    CHECK(c.copy_selection() && cb.has_image());
    uint32_t cw, ch;
    CHECK(cb.image_size(&cw, &ch) && cw == 10 && ch == 10);
    uint32_t part[100];
    CHECK(cb.get_image(part, 100) && part[0] == red && part[99] == red);
    c.clear_selection();
    CHECK(c.copy_selection() && cb.image_size(&cw, &ch) && cw == 40 && ch == 30);
    CHECK(!c.cut_selection());                  // nothing selected: nothing to cut

    // Cut clears the area to the secondary color; undo brings it back.
    c.select_rect(5, 5, 10, 10);
    CHECK(c.cut_selection() && c.pixel(7, 7) == white && c.pixel(14, 14) == white && c.has_selection());   // the rectangle stays
    CHECK(cb.image_size(&cw, &ch) && cw == 10 && cb.get_image(part, 100) && part[55] == red);
    c.undo();
    CHECK(c.pixel(7, 7) == red);

    // Paste: a floating picture appears in the corner of the view, the tool becomes Select, and it can be dragged.
    const int32_t tool_events = tools.count;
    c.set_tool(PaintTool::Pencil);
    CHECK(c.paste() && c.floating() && c.has_selection() && c.tool() == PaintTool::Select && tools.count == tool_events + 1);
    CHECK(c.selection().x == 0 && c.selection().y == 0 && c.selection().width == 10 && c.pixel(3, 3) == red && c.pixel(12, 12) == red && c.modified());
    w.mouse_move(pixel_point(c, 3, 3).x, pixel_point(c, 3, 3).y);
    w.mouse_button(pixel_point(c, 3, 3).x, pixel_point(c, 3, 3).y, MouseButton::Left, true);
    w.mouse_move(pixel_point(c, 23, 13).x, pixel_point(c, 23, 13).y);
    w.mouse_button(pixel_point(c, 23, 13).x, pixel_point(c, 23, 13).y, MouseButton::Left, false);
    CHECK(c.floating() && c.selection().x == 20 && c.selection().y == 10 && c.pixel(3, 3) == white && c.pixel(25, 15) == red && c.pixel(19, 15) == white);
    press(w, key::kEnter);                      // set down
    CHECK(!c.floating() && c.pixel(25, 15) == red && c.has_selection());
    c.undo();                                   // one step undoes the whole paste
    CHECK(c.pixel(25, 15) == white && c.pixel(7, 7) == red && c.can_undo());
    c.redo();
    CHECK(c.pixel(25, 15) == red);
    c.undo();

    // Escape takes a pasted picture back.
    CHECK(c.paste() && c.floating() && c.pixel(2, 2) == red);
    press(w, key::kEscape);
    CHECK(!c.floating() && !c.has_selection() && c.pixel(2, 2) == white && c.pixel(7, 7) == red);
    // Undo does the same.
    CHECK(c.paste());
    c.undo();
    CHECK(!c.floating() && c.pixel(2, 2) == white);
    // A pasted picture is set down when another tool is used.
    CHECK(c.paste());
    c.set_tool(PaintTool::Pencil);
    CHECK(!c.floating() && c.pixel(2, 2) == red);
    c.undo();

    // Dragging inside a selection lifts it and carries it; the hole takes the secondary color.
    c.set_tool(PaintTool::Select);
    c.select_rect(5, 5, 10, 10);
    w.mouse_move(pixel_point(c, 7, 7).x, pixel_point(c, 7, 7).y);
    w.mouse_button(pixel_point(c, 7, 7).x, pixel_point(c, 7, 7).y, MouseButton::Left, true);
    CHECK(c.floating() && c.pixel(7, 7) == red);      // lifted, but still where it was
    w.mouse_move(pixel_point(c, 27, 17).x, pixel_point(c, 27, 17).y);
    w.mouse_button(pixel_point(c, 27, 17).x, pixel_point(c, 27, 17).y, MouseButton::Left, false);
    CHECK(c.selection().x == 25 && c.selection().y == 15 && c.pixel(7, 7) == white && c.pixel(5, 5) == white && c.pixel(30, 20) == red);
    click(w, pixel_point(c, 1, 28).x, pixel_point(c, 1, 28).y);   // outside: set down, and start a new selection
    CHECK(!c.floating() && c.pixel(30, 20) == red && !c.has_selection());
    c.undo();
    CHECK(c.pixel(7, 7) == red && c.pixel(30, 20) == white);

    // Delete clears a selection; with a floating picture it drops the picture.
    c.select_rect(5, 5, 10, 10);
    press(w, 0x4C);
    CHECK(c.pixel(7, 7) == white && c.has_selection());
    c.undo();
    CHECK(c.paste());
    press(w, 0x4C);
    CHECK(!c.floating() && c.pixel(2, 2) == white);
    c.undo();
    CHECK(c.pixel(7, 7) == red);

    // Translucent pixels are blended over what is under them; a picture bigger than the bitmap is cut off.
    const uint32_t soft[4] = {0x80000000, 0xFF00FF00, 0x00000000, 0xFF0000FF};
    CHECK(cb.set_image(soft, 2, 2) && c.paste());
    CHECK(red_of(c.pixel(0, 0)) > 0x70 && red_of(c.pixel(0, 0)) < 0x90 && c.pixel(1, 0) == 0xFF00FF00 && c.pixel(0, 1) == white && c.pixel(1, 1) == blue);
    c.undo();
    uint32_t* big = static_cast<uint32_t*>(malloc(60 * 50 * 4));
    for (int i = 0; i < 60 * 50; ++i) big[i] = blue;
    CHECK(cb.set_image(big, 60, 50) && c.paste() && c.pixel(39, 29) == blue && c.pixel(0, 0) == blue);
    c.undo();
    free(big);
    CHECK(cb.set_text("text") && !c.paste());  // text is not a picture
    c.clear_selection();
    w.render();

    // Ctrl+C, Ctrl+X and Ctrl+V on the canvas, and Shift+Insert.
    CHECK(cb.set_image(soft, 2, 2));
    c.select_rect(5, 5, 4, 4);
    press(w, key::kA + 2, modifier::kLeftControl);
    CHECK(cb.image_size(&cw, &ch) && cw == 4 && ch == 4);
    press(w, key::kA + 21, modifier::kLeftControl);
    CHECK(c.floating());
    press(w, key::kEnter);
    press(w, 0x4A, modifier::kLeftControl);
    c.select_rect(5, 5, 4, 4);
    press(w, key::kA + 23, modifier::kLeftControl);
    CHECK(c.pixel(6, 6) == white && c.has_selection());
    press(w, 0x49, modifier::kLeftShift);
    CHECK(c.floating());
    c.clear_selection();
    CHECK(!c.floating());
  }
  CHECK(a.live == 0);
}

static void test_paint_clipboard() {
  TestAllocator a;
  {
    kernel::Clipboard cb(a);
    filesystem::RamFileSystem fs(a);
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    w.set_clipboard(&cb);
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::Paint paint(a);
    paint.set_file_system(&fs);
    CHECK(mgr.add_window(&paint));
    w.layout();
    DrawingCanvas& c = paint.canvas();
    paint.palette().set_primary(0xFFFF0000);
    drag(w, pixel_point(c, 10, 10), pixel_point(c, 30, 10));
    CHECK(c.pixel(20, 10) == 0xFFFF0000);

    CHECK(choose(w, paint.menu_bar(), "Edit", "Cut"));                 // nothing selected
    CHECK(paint.status_label().text().equals("Select something to cut first."));
    CHECK(choose(w, paint.menu_bar(), "Edit", "Copy"));                // so the whole picture is copied
    uint32_t cw, ch;
    CHECK(paint.status_label().text().equals("Copied the whole picture.") && cb.image_size(&cw, &ch) && cw == 640 && ch == 480);

    paint.tools().select_tool(7);
    CHECK(c.tool() == PaintTool::Select);
    c.select_rect(8, 8, 30, 6);
    CHECK(choose(w, paint.menu_bar(), "Edit", "Copy"));
    CHECK(paint.status_label().text().equals("Copied the selection.") && cb.image_size(&cw, &ch) && cw == 30 && ch == 6);
    CHECK(choose(w, paint.menu_bar(), "Edit", "Cut"));
    CHECK(paint.status_label().text().equals("Cut to the clipboard.") && c.pixel(20, 10) == 0xFFFFFFFF);

    // Paste: a floating picture, and the tool strip follows the canvas to the Select tool.
    paint.tools().select_tool(0);
    CHECK(choose(w, paint.menu_bar(), "Edit", "Paste"));
    CHECK(c.floating() && paint.tools().selected_tool() == 7 && c.pixel(5, 3) == 0xFFFFFFFF && c.pixel(2, 3) == 0xFFFFFFFF);
    CHECK(paint.status_label().text().first(7).equals("Pasted."));
    // Saving sets it down first.
    CHECK(paint.save_file("/p.png") && !c.floating());
    // Ctrl+V from somewhere that is not the canvas goes through the window.
    w.set_focus(&paint.palette());
    press(w, key::kA + 21, modifier::kLeftControl);
    CHECK(c.floating());
    c.undo();
    CHECK(cb.set_text("words"));
    CHECK(choose(w, paint.menu_bar(), "Edit", "Paste"));
    CHECK(!c.floating() && paint.status_label().text().equals("Nothing to paste: the clipboard has no picture."));
    w.render();
    mgr.remove_window(&paint);
  }
  CHECK(a.live == 0);
}

static void test_list_copy() {
  TestAllocator a;
  {
    kernel::Clipboard cb(a);
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    w.set_clipboard(&cb);
    ListView list(a);
    DataGrid grid(a);
    list.set_bounds({10, 10, 300, 100});
    grid.set_bounds({10, 120, 300, 100});
    w.add(&list);
    w.add(&grid);
    fill_list(list, 4, 1);
    grid.add_column("A", 60);
    grid.add_column("B", 60);
    const StringView r0[2] = {"x", "y"}, r1[2] = {"left", "right"};
    grid.add_row(r0, 2);
    grid.add_row(r1, 2);
    w.layout();
    String got(a);
    w.set_focus(&list);
    press(w, key::kA + 2, modifier::kLeftControl);       // nothing selected: nothing copied
    CHECK(!cb.get_text(&got));
    list.select_item(2);
    press(w, key::kA + 2, modifier::kLeftControl);
    CHECK(cb.get_text(&got) && got.view().equals("item02\t1 KB"));
    list.set_copy_enabled(false);
    CHECK(cb.set_text("kept"));
    press(w, key::kA + 2, modifier::kLeftControl);
    CHECK(cb.get_text(&got) && got.view().equals("kept"));
    w.set_focus(&grid);
    grid.select_row(1);
    press(w, key::kA + 2, modifier::kLeftControl);
    CHECK(cb.get_text(&got) && got.view().equals("left\tright"));
  }
  CHECK(a.live == 0);
}

static void test_select_all_keys() {
  TestAllocator a;
  {
    kernel::Clipboard cb(a);
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    w.set_clipboard(&cb);
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::Paint paint(a);
    CHECK(mgr.add_window(&paint));
    w.layout();
    // Ctrl+A in Paint selects the whole picture and switches to the Select tool, wherever the focus is.
    w.set_focus(&paint.palette());
    CHECK(paint.canvas().tool() == PaintTool::Pencil);
    press(w, key::kA, modifier::kLeftControl);
    CHECK(paint.canvas().has_selection() && paint.canvas().selection().width == 640 && paint.canvas().tool() == PaintTool::Select && paint.tools().selected_tool() == 7);
    press(w, key::kA + 2, modifier::kLeftControl);
    uint32_t cw, ch;
    CHECK(cb.image_size(&cw, &ch) && cw == 640 && ch == 480);
    paint.tools().select_tool(0);
    w.set_focus(&paint.canvas());
    press(w, key::kA, modifier::kLeftControl);
    CHECK(paint.tools().selected_tool() == 7);
    mgr.remove_window(&paint);
  }
  CHECK(a.live == 0);
}

// --- Menu bars of the applications ------------------------------------------------------------------------------

static void test_app_menus() {
  TestAllocator a;
  {
    kernel::Clipboard cb(a);
    filesystem::RamFileSystem fs(a);
    CHECK(fs.create_directory("/docs") == filesystem::Status::Ok);
    CHECK(fs.create_directory("/docs/old") == filesystem::Status::Ok);
    make_file(fs, "/a.txt", "abc");
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    w.set_clipboard(&cb);
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    String got(a);

    // --- Text editor
    {
      desktop::Notepad np(a);
      np.set_file_system(&fs);
      CHECK(mgr.add_window(&np));
      w.layout();
      MenuBar& bar = np.menu_bar();
      CHECK(bar.menu_count() == 2 && bar.menu(0)->title().equals("File") && bar.menu(1)->title().equals("Edit") && bar.parent() == &np);
      CHECK(bar.bounds().y < np.path_box().bounds().y + 40 && bar.bounds().width > 100 && bar.window_origin().y < np.editor().window_origin().y);
      CHECK(bar.find_item("File", "Save") && bar.find_item("File", "Save")->shortcut.view().equals("Ctrl+S") && !bar.find_item("File", "Nope"));
      MenuItem* cut = bar.find_item("Edit", "Cut");
      MenuItem* copy = bar.find_item("Edit", "Copy");
      CHECK(!cut->enabled() && !copy->enabled());      // nothing selected
      w.set_focus(&np.editor());
      type_text(w, "hello");
      CHECK(choose(w, bar, "Edit", "Select All"));
      CHECK(np.editor().has_selection() && cut->enabled() && copy->enabled());
      CHECK(choose(w, bar, "Edit", "Copy") && cb.get_text(&got) && got.view().equals("hello") && np.editor().text().equals("hello"));
      CHECK(choose(w, bar, "Edit", "Cut") && np.editor().text().empty() && !cut->enabled());
      CHECK(!choose(w, bar, "Edit", "Cut") && w.popup() == nullptr);   // disabled: nothing happens, the menu closes
      CHECK(choose(w, bar, "Edit", "Paste") && np.editor().text().equals("hello"));
      // File > Save writes to the path box; New asks first when there are unsaved changes.
      np.path_box().set_text("note.txt");
      CHECK(choose(w, bar, "File", "Save") && np.status_label().text().equals("Saved /note.txt") && !np.editor().modified());
      CHECK(choose(w, bar, "File", "New") && np.editor().text().empty() && np.path().empty());
      np.path_box().set_text("/a.txt");
      CHECK(choose(w, bar, "File", "Open") && np.editor().text().equals("abc"));
      press(w, key::kA + 18, modifier::kLeftControl);    // the shortcut still works
      CHECK(np.status_label().text().equals("Saved /a.txt"));
      CHECK(choose(w, bar, "File", "Close") && np.manager() == nullptr && mgr.window_count() == 0);
    }

    // --- File explorer
    {
      desktop::FileExplorer ex(a);
      ex.set_file_system(&fs);
      CHECK(mgr.add_window(&ex));
      w.layout();
      MenuBar& bar = ex.menu_bar();
      CHECK(bar.menu_count() == 3 && bar.menu(2)->title().equals("Go") && ex.details_item()->checked() && !ex.list_item()->checked());
      CHECK(choose(w, bar, "View", "Icons") && ex.view().view_mode() == ViewMode::Icons && ex.icons_item()->checked() && !ex.details_item()->checked());
      CHECK(choose(w, bar, "View", "Details") && ex.view().view_mode() == ViewMode::Details);
      ex.view().select_item(0);                        // "docs"
      CHECK(choose(w, bar, "File", "Open") && ex.path().equals("/docs"));
      CHECK(choose(w, bar, "Go", "Up One Level") && ex.path().equals("/"));
      CHECK(!choose(w, bar, "Go", "Up One Level") || ex.path().equals("/"));   // at the root nothing changes
      ex.view().select_item(1);
      CHECK(choose(w, bar, "File", "Copy Path") && cb.get_text(&got) && got.view().equals("/a.txt"));
      CHECK(choose(w, bar, "Go", "Address") && ex.address_bar().editing());
      press(w, key::kEscape);
      CHECK(!ex.address_bar().editing());
      const uint32_t before = ex.view().item_count();
      CHECK(before == 3 && choose(w, bar, "View", "Refresh") && ex.view().item_count() == before);   // nothing changed yet
      make_file(fs, "/c.txt", "newer");
      CHECK(choose(w, bar, "View", "Refresh") && ex.view().item_count() == before + 1);
      CHECK(choose(w, bar, "File", "Close") && ex.manager() == nullptr);
    }

    // --- Task manager
    {
      desktop::TaskManager tm(a);
      CHECK(mgr.add_window(&tm));
      w.layout();
      MenuBar& bar = tm.menu_bar();
      CHECK(bar.menu_count() == 2 && bar.find_item("View", "Processes")->checked() && !bar.find_item("View", "Performance")->checked());
      CHECK(choose(w, bar, "View", "Performance") && tm.tabs().selected_page() == 1 && bar.find_item("View", "Performance")->checked() && !bar.find_item("View", "Processes")->checked());
      tm.tabs().select_page(0);                        // switching the tab by hand moves the check mark too
      CHECK(bar.find_item("View", "Processes")->checked() && !bar.find_item("View", "Performance")->checked());
      CHECK(choose(w, bar, "View", "Processes") && tm.tabs().selected_page() == 0);
      CHECK(choose(w, bar, "File", "Close") && tm.manager() == nullptr);
    }

    // --- Paint
    {
      desktop::Paint paint(a);
      paint.set_file_system(&fs);
      CHECK(mgr.add_window(&paint));
      w.layout();
      MenuBar& bar = paint.menu_bar();
      DrawingCanvas& c = paint.canvas();
      CHECK(bar.menu_count() == 4 && bar.menu(3)->title().equals("Options") && bar.find_item("Edit", "Paste")->shortcut.view().equals("Ctrl+V"));
      CHECK(!bar.find_item("Edit", "Undo")->enabled() && !choose(w, bar, "Edit", "Undo"));
      drag(w, pixel_point(c, 10, 10), pixel_point(c, 30, 10));
      CHECK(bar.find_item("Edit", "Undo")->enabled());
      CHECK(choose(w, bar, "Edit", "Select All") && c.has_selection() && paint.tools().selected_tool() == 7);
      CHECK(choose(w, bar, "View", "Zoom In") && c.zoom() == 2 && paint.zoom_label().text().equals("200%"));
      press(w, 0x2E, modifier::kLeftControl);          // Ctrl+= zooms in, Ctrl+- out, Ctrl+0 resets
      CHECK(c.zoom() == 3);
      press(w, 0x2D, modifier::kLeftControl);
      CHECK(c.zoom() == 2);
      CHECK(choose(w, bar, "View", "Actual Size") && c.zoom() == 1);
      paint.path_box().set_text("pic");
      CHECK(choose(w, bar, "File", "Save") && paint.path().equals("/pic.png") && paint.title().equals("Paint - pic.png"));
      paint.tools().select_tool(0);
      drag(w, pixel_point(c, 50, 50), pixel_point(c, 60, 50));
      CHECK(choose(w, bar, "File", "New") && paint.status_label().text().first(13).equals("Unsaved chang") && c.pixel(55, 50) != 0xFFFFFFFF);
      CHECK(choose(w, bar, "File", "New") && c.pixel(55, 50) == 0xFFFFFFFF && c.image_width() == 640);
      CHECK(choose(w, bar, "File", "Open") == true);   // the path box was cleared by New: asks for a path
      CHECK(paint.status_label().text().equals("Enter the path of a picture to open."));
      CHECK(choose(w, bar, "File", "Close") && paint.manager() == nullptr);
    }
    w.render();
  }
  CHECK(a.live == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not lose the report of the checks that failed before it
  test_scroll_bar();
  test_data_grid();
  test_data_grid_scrolling();
  test_grid_rendering();
  test_image_view();
  test_player_control();
  test_video_control();
  test_list_view();
  test_tab_control();
  test_tool_strip();
  test_color_palette();
  test_drawing_canvas();
  test_paint();
  test_text_clipboard();
  test_canvas_clipboard();
  test_paint_clipboard();
  test_list_copy();
  test_app_menus();
  test_select_all_keys();
  test_breadcrumb_bar();
  test_file_explorer();
  test_task_manager();
  test_text_editor();
  test_notepad();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gui views: all checks passed\n");
  return 0;
}
