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
#include <string>
#include <vector>
#include <cstdlib>
#include <stdlib.h>
#include <string.h>
#include "Audio/AudioPlayer.hpp"
#include "Media/AudioStreaming.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Gui/Controls/AudioPlayerControl.hpp"
#include "Gui/Color.hpp"
#include "Kernel/Clipboard.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Desktop/FileExplorer.hpp"
#include "Desktop/FileTransfer.hpp"
#include "Desktop/CopyProgressDialog.hpp"
#include "Desktop/ColorDialog.hpp"
#include "Desktop/Desktop.hpp"
#include "Desktop/PrintDialog.hpp"
#include "Desktop/FileDialog.hpp"
#include "Desktop/MediaPlayer.hpp"
#include "Desktop/Notepad.hpp"
#include "Desktop/Terminal.hpp"
#include "Desktop/Paint.hpp"
#include "Desktop/ComputerManager.hpp"
#include "Gui/Controls/TextEditor.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Gui/Controls/BreadcrumbBar.hpp"
#include "Gui/Controls/ColorPalette.hpp"
#include "Gui/Controls/DrawingCanvas.hpp"
#include "Gui/Controls/ToolStrip.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/ImageView.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Gui/Controls/ColorSpectrum.hpp"
#include "Gui/Controls/ScrollBar.hpp"
#include "Gui/Controls/SpinBox.hpp"
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

// Clicks the row of `item` in the menu shown as the window's popup. False if there is no popup or no such item, or the item is disabled (the
// click then does nothing and the popup is closed).
static bool click_popup_item(Window& w, Menu* m, const char* item) {
  Control* popup = w.popup();
  if (!popup || !m) return false;
  int32_t top = 2;
  for (uint32_t i = 0; i < m->item_count(); ++i) {
    const int32_t height = m->item(i)->separator ? MenuPopup::kSeparatorHeight : MenuPopup::kRowHeight;
    if (!m->item(i)->separator && m->item(i)->text.view().equals(item)) {
      const bool enabled = m->item(i)->enabled();
      click(w, popup->bounds().x + 30, popup->bounds().y + top + height / 2);
      if (w.popup()) w.close_popup();   // a disabled item keeps the menu open
      return enabled;
    }
    top += height;
  }
  w.close_popup();
  return false;
}

// Chooses a menu item the way a user does: clicks the title, then the item's row in the drop-down. False if there is no such
// menu or item, or the item is disabled (the click then does nothing).
static bool choose(Window& w, MenuBar& bar, const char* menu, const char* item) {
  int32_t index = -1;
  for (uint32_t m = 0; m < bar.menu_count(); ++m)
    if (bar.menu(m)->title().equals(menu)) index = static_cast<int32_t>(m);
  if (index < 0 || !bar.find_item(menu, item)) return false;
  bar.open_menu(index);
  return click_popup_item(w, bar.menu(static_cast<uint32_t>(index)), item);
}

// Presses and releases the right button at (x, y) of the window, which opens the context menu there.
static void right_click(Window& w, int32_t x, int32_t y) {
  w.mouse_move(x, y);
  w.mouse_button(x, y, MouseButton::Right, true);
  w.mouse_button(x, y, MouseButton::Right, false);
}

// Right-clicks at (x, y) and chooses `item` in the menu that opens. False if no menu opened or the item cannot be chosen.
static bool choose_context(Window& w, ContextMenu& menu, int32_t x, int32_t y, const char* item) {
  right_click(w, x, y);
  return w.popup() && menu.open() && click_popup_item(w, &menu.menu(), item);
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
    CHECK(player.playing() && !panel.play_button().enabled() && panel.pause_button().enabled());
    step(12);
    CHECK(panel.seek_slider().value() > 0);
    CHECK(panel.meter().level() > 50);   // a loud tone
    w.render();

    // Pause keeps the place.
    click_center(w, panel.pause_button());
    CHECK(player.state() == audio::PlayerState::Paused && panel.play_button().enabled() && !panel.pause_button().enabled());
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
    CHECK(panel.position_label().text().equals("00:00") && panel.length_label().text().equals("00:02"));

    // Fast forward skips ahead (here: to the end of the short clip), rewind goes back (to the start).
    click_center(w, panel.forward_button());
    CHECK(player.position() >= player.duration() - 1 && panel.position_label().text().equals("00:02"));
    click_center(w, panel.rewind_button());
    CHECK(player.position() == 0 && panel.position_label().text().equals("00:00"));

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
    CHECK(player.state() == audio::PlayerState::Stopped && panel.time_text().equals("00:02 / 00:02") && panel.play_button().enabled() && !panel.pause_button().enabled());
    CHECK(panel.seek_slider().value() == 1000);

    player.close();
    panel.refresh();
    CHECK(!panel.play_button().enabled() && !panel.rewind_button().enabled() && panel.time_text().equals("00:00 / 00:00"));
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
    CHECK(player.playing() && !panel.play_button().enabled() && panel.pause_button().enabled());
    step(12);
    CHECK(panel.seek_slider().value() > 0);
    w.render();
    const int red = (int)((screen.px(cx, cy) >> 16) & 0xFF);
    CHECK(red >= 5 + 10 * (int)player.current_frame() - 6 && red <= 5 + 10 * (int)player.current_frame() + 6 && player.current_frame() > 0);

    click_center(w, panel.pause_button());
    CHECK(player.state() == video::VideoState::Paused && panel.play_button().enabled() && !panel.pause_button().enabled());
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

    CHECK(panel.position_label().text().equals("00:00") && panel.length_label().text().equals("00:02"));
    // Fast forward and rewind skip by ten seconds, stopping at the end and the start of the short clip.
    click_center(w, panel.forward_button());
    CHECK(player.position_us() >= player.duration_us() - 200000 && panel.position_label().text().equals("00:02"));
    click_center(w, panel.rewind_button());
    CHECK(player.position_us() == 0 && panel.position_label().text().equals("00:00"));

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

struct EjectProbe {
  char volume[64] = {};
  int calls = 0;
  const char* answer = "ejected";
};
static const char* probe_eject(void* context, StringView volume, bool*) {
  EjectProbe* p = static_cast<EjectProbe*>(context);
  ++p->calls;
  const uint64_t n = volume.size() < sizeof p->volume - 1 ? volume.size() : sizeof p->volume - 1;
  memcpy(p->volume, volume.data(), n);
  p->volume[n] = 0;
  return p->answer;
}

// A filesystem that locks the folders of the "Network" tree until it has been given the right password, like a server that wants a login.
struct LockedNetworkFs final : filesystem::IFileSystem {
  explicit LockedNetworkFs(filesystem::RamFileSystem& inner) : ram(inner) {}
  filesystem::RamFileSystem& ram;
  bool unlocked = false;
  const char* locked_prefix = "/Network/srv";
  filesystem::Status failure = filesystem::Status::LoginRequired;
  filesystem::FileSystemInfo info() const override { return ram.info(); }
  filesystem::Status gate(StringView path) const { return path.starts_with(StringView(locked_prefix)) && !unlocked ? failure : filesystem::Status::Ok; }
  filesystem::Status open_file(StringView p, filesystem::OpenMode m, UniquePointer<filesystem::IFile>* f) override { return ram.open_file(p, m, f); }
  filesystem::Status open_directory(StringView p, UniquePointer<filesystem::IDirectory>* d) override {
    const filesystem::Status g = gate(p);
    return g != filesystem::Status::Ok ? g : ram.open_directory(p, d);
  }
  filesystem::Status stat(StringView p, filesystem::DirectoryEntry* e) override { return ram.stat(p, e); }
  filesystem::Status create_directory(StringView p) override { return ram.create_directory(p); }
  filesystem::Status remove(StringView p) override { return ram.remove(p); }
  filesystem::Status rename(StringView a, StringView b) override { return ram.rename(a, b); }
  filesystem::Status flush() override { return ram.flush(); }
};

struct LoginRecord {
  LockedNetworkFs* fs = nullptr;
  int calls = 0;
  std::string server, user, password;
};
static bool record_login(void* context, StringView server, StringView user, StringView password) {
  auto* r = static_cast<LoginRecord*>(context);
  ++r->calls;
  r->server.assign(server.data(), server.size());
  r->user.assign(user.data(), user.size());
  r->password.assign(password.data(), password.size());
  r->fs->unlocked = r->password == "secret";
  return true;
}

static void test_explorer_network() {
  TestAllocator a;
  {
    filesystem::RamFileSystem ram(a);
    CHECK(ram.create_directory("/Network") == filesystem::Status::Ok && ram.create_directory("/Network/srv") == filesystem::Status::Ok);
    CHECK(ram.create_directory("/Network/srv/share") == filesystem::Status::Ok && ram.create_directory("/Network/srv/share/docs") == filesystem::Status::Ok);
    make_file(ram, "/Network/srv/share/a.txt", "hello");
    LockedNetworkFs fs(ram);
    LoginRecord record;
    record.fs = &fs;

    // Addresses: smb:// and UNC forms become paths in the tree; a login in the address comes out separately.
    {
      String path(a), user(a), password(a);
      using desktop::FileExplorer;
      CHECK(FileExplorer::parse_network_address(a, "smb://srv/share/docs", &path, &user, &password) && path.view().equals("/Network/srv/share/docs") && user.empty());
      CHECK(FileExplorer::parse_network_address(a, "SMB://srv", &path, &user, &password) && path.view().equals("/Network/srv"));
      CHECK(FileExplorer::parse_network_address(a, "smb://", &path, &user, &password) && path.view().equals("/Network"));
      CHECK(FileExplorer::parse_network_address(a, "smb://srv:4455//share///docs/", &path, &user, &password) && path.view().equals("/Network/srv:4455/share/docs"));
      CHECK(FileExplorer::parse_network_address(a, "smb://al%40ice:p%3Ass@srv/share", &path, &user, &password) && path.view().equals("/Network/srv/share") && user.view().equals("al@ice") && password.view().equals("p:ss"));
      CHECK(FileExplorer::parse_network_address(a, "smb://bob@srv/share", &path, &user, &password) && user.view().equals("bob") && password.empty());
      CHECK(FileExplorer::parse_network_address(a, "\\\\srv\\share\\docs", &path, &user, &password) && path.view().equals("/Network/srv/share/docs"));
      CHECK(!FileExplorer::parse_network_address(a, "/Network/srv", &path, &user, &password) && !FileExplorer::parse_network_address(a, "http://srv/", &path, &user, &password));
    }

    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::FileExplorer ex(a);
    ex.set_file_system(&fs);
    ex.set_login_handler(record_login, &record);
    CHECK(mgr.add_window(&ex));
    w.layout();
    desktop::CredentialsDialog& dialog = ex.credentials_dialog();

    // A locked server asks for a login; a wrong password asks again, saying so; the right one opens the folder.
    CHECK(!ex.go_to("smb://srv/share") && dialog.showing() && dialog.server().equals("srv") && dialog.note_label().text().empty());
    CHECK(ex.path().equals("/") && dialog.modal() && w.focus() == &dialog.user_box());
    dialog.user_box().set_text("alice");
    w.set_focus(&dialog.password_box());
    dialog.password_box().set_text("wrong");
    CHECK(dialog.password_box().password());
    fs.failure = filesystem::Status::LoginFailed;   // (what the server answers to a wrong login)
    click_center(w, dialog.ok_button());
    CHECK(record.calls == 1 && record.server == "srv" && record.user == "alice" && record.password == "wrong");
    CHECK(dialog.showing() && dialog.user().equals("alice") && dialog.note_label().text().size() > 8 && dialog.note_label().text().first(8).equals("The user"));
    dialog.password_box().set_text("secret");
    press(w, key::kEnter);
    CHECK(!dialog.showing() && record.calls == 2 && record.password == "secret" && dialog.password().empty());
    CHECK(ex.path().equals("/Network/srv/share") && ex.view().item_count() == 2);
    // Cancel hands nothing over.
    fs.unlocked = false;
    fs.failure = filesystem::Status::LoginRequired;
    CHECK(!ex.go_to("smb://srv/share/docs") && dialog.showing());
    click_center(w, dialog.cancel_button());
    CHECK(!dialog.showing() && record.calls == 2 && ex.path().equals("/Network/srv/share"));
    // A login written in the address goes straight to the handler.
    CHECK(ex.go_to("smb://carol:secret@srv/share/docs") && record.calls == 3 && record.user == "carol" && ex.path().equals("/Network/srv/share/docs"));
    // Typing the address in the address bar works the same, and so does the menu's way in.
    fs.unlocked = true;
    ex.address_bar().begin_edit();
    ex.address_bar().edit_box().set_text("smb://srv/share");
    w.set_focus(&ex.address_bar().edit_box());
    press(w, key::kEnter);
    CHECK(ex.path().equals("/Network/srv/share"));
    ex.address_bar().begin_edit("smb://");
    CHECK(ex.address_bar().editing() && ex.address_bar().edit_box().text().equals("smb://"));
    press(w, key::kEscape);
    // Other kinds of failure are told as errors, not asked for a login.
    fs.unlocked = false;
    fs.failure = filesystem::Status::Network;
    ex.address_bar().begin_edit();
    ex.address_bar().edit_box().set_text("smb://srv/other");
    w.set_focus(&ex.address_bar().edit_box());
    press(w, key::kEnter);
    CHECK(!dialog.showing() && ex.status_label().text().find("Cannot open") != StringView::npos);
    mgr.remove_window(&ex);
  }
  CHECK(a.live == 0);
}

// NFS: its own folder, addresses with user and group ids, and a dialog that asks for ids instead of a password.
static bool record_nfs_login(void* context, StringView server, StringView uid, StringView gid) {
  auto* r = static_cast<LoginRecord*>(context);
  ++r->calls;
  r->server.assign(server.data(), server.size());
  r->user.assign(uid.data(), uid.size());
  r->password.assign(gid.data(), gid.size());
  r->fs->unlocked = r->user == "1000";
  return true;
}

static void test_explorer_nfs() {
  TestAllocator a;
  {
    using desktop::FileExplorer;
    String path(a), user(a), password(a);
    CHECK(FileExplorer::parse_network_address(a, "nfs://srv/export/data", &path, &user, &password) && path.view().equals("/NFS/srv/export/data") && user.empty());
    CHECK(FileExplorer::parse_network_address(a, "NFS://srv:2049//export///data/", &path, &user, &password) && path.view().equals("/NFS/srv:2049/export/data"));
    CHECK(FileExplorer::parse_network_address(a, "nfs://", &path, &user, &password) && path.view().equals("/NFS"));
    CHECK(FileExplorer::parse_network_address(a, "nfs://1000:100@srv/export", &path, &user, &password) && path.view().equals("/NFS/srv/export") && user.view().equals("1000") && password.view().equals("100"));
    CHECK(!FileExplorer::parse_network_address(a, "/NFS/srv", &path, &user, &password));

    filesystem::RamFileSystem ram(a);
    CHECK(ram.create_directory("/NFS") == filesystem::Status::Ok && ram.create_directory("/NFS/srv") == filesystem::Status::Ok);
    CHECK(ram.create_directory("/NFS/srv/export") == filesystem::Status::Ok && ram.create_directory("/NFS/srv/export/data") == filesystem::Status::Ok);
    make_file(ram, "/NFS/srv/export/data/a.txt", "hello");
    LockedNetworkFs fs(ram);
    fs.locked_prefix = "/NFS/srv";
    fs.failure = filesystem::Status::AccessDenied;
    LoginRecord record;
    record.fs = &fs;

    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::FileExplorer ex(a);
    ex.set_file_system(&fs);
    ex.set_nfs_login_handler(record_nfs_login, &record);
    CHECK(mgr.add_window(&ex));
    w.layout();
    desktop::CredentialsDialog& dialog = ex.credentials_dialog();

    // Denied: the dialog asks for ids (65534 to start with, in plain text) and says why; other ids open the folder.
    CHECK(!ex.go_to("nfs://srv/export/data") && dialog.showing() && dialog.server().equals("srv"));
    CHECK(dialog.user().equals("65534") && dialog.password().equals("65534") && !dialog.password_box().password());
    CHECK(dialog.note_label().text().size() > 6 && dialog.note_label().text().first(6).equals("Access"));
    dialog.user_box().set_text("1000");
    dialog.password_box().set_text("100");
    click_center(w, dialog.ok_button());
    CHECK(!dialog.showing() && record.calls == 1 && record.server == "srv" && record.user == "1000" && record.password == "100");
    CHECK(ex.path().equals("/NFS/srv/export/data") && ex.view().item_count() == 1);
    // Ids in the address go straight to the handler.
    fs.unlocked = false;
    CHECK(ex.go_to("nfs://1000:5@srv/export") && record.calls == 2 && record.password == "5" && ex.path().equals("/NFS/srv/export"));
    // The next sign-in remembers what was typed.
    fs.unlocked = false;
    CHECK(!ex.go_to("nfs://srv/export/data") && dialog.showing() && dialog.user().equals("1000") && dialog.password().equals("100"));
    click_center(w, dialog.cancel_button());
    CHECK(!dialog.showing() && record.calls == 2);
    mgr.remove_window(&ex);
  }
  CHECK(a.live == 0);
}

// SFTP and FTPS: their own folders and addresses, signed in to with a user name and a password.
static void test_explorer_sftp_ftps() {
  TestAllocator a;
  {
    using desktop::FileExplorer;
    String path(a), user(a), password(a);
    CHECK(FileExplorer::parse_network_address(a, "sftp://srv/home/me", &path, &user, &password) && path.view().equals("/SFTP/srv/home/me") && user.empty());
    CHECK(FileExplorer::parse_network_address(a, "SFTP://me:p%40ss@srv:2222/", &path, &user, &password) && path.view().equals("/SFTP/srv:2222") && user.view().equals("me") && password.view().equals("p@ss"));
    CHECK(FileExplorer::parse_network_address(a, "ftps://srv/pub//docs/", &path, &user, &password) && path.view().equals("/FTPS/srv/pub/docs"));
    CHECK(FileExplorer::parse_network_address(a, "ftps://srv:990", &path, &user, &password) && path.view().equals("/FTPS/srv:990"));
    CHECK(FileExplorer::parse_network_address(a, "sftp://", &path, &user, &password) && path.view().equals("/SFTP"));
    CHECK(!FileExplorer::parse_network_address(a, "ftp://srv/", &path, &user, &password) && !FileExplorer::parse_network_address(a, "/SFTP/srv", &path, &user, &password));

    for (int round = 0; round < 2; ++round) {
      const bool sftp = round == 0;
      const char* folder = sftp ? "SFTP" : "FTPS";
      filesystem::RamFileSystem ram(a);
      String base(a);
      base.append("/");
      base.append(folder);
      CHECK(ram.create_directory(base.view()) == filesystem::Status::Ok);
      base.append("/srv");
      CHECK(ram.create_directory(base.view()) == filesystem::Status::Ok);
      String file(a);
      file.assign(base.view());
      file.append("/a.txt");
      make_file(ram, file.c_str(), "hello");
      LockedNetworkFs fs(ram);
      fs.locked_prefix = sftp ? "/SFTP/srv" : "/FTPS/srv";
      fs.failure = filesystem::Status::LoginRequired;
      LoginRecord record;
      record.fs = &fs;

      Screen screen(a, 640, 480);
      Theme theme;
      Window w(a, screen.renderer, theme, {640, 480});
      WindowManager mgr(a);
      mgr.set_dock(DockStyle::Fill);
      w.add(&mgr);
      desktop::FileExplorer ex(a);
      ex.set_file_system(&fs);
      if (sftp) ex.set_sftp_login_handler(record_login, &record);
      else ex.set_ftps_login_handler(record_login, &record);
      CHECK(mgr.add_window(&ex));
      w.layout();
      desktop::CredentialsDialog& dialog = ex.credentials_dialog();
      CHECK(!ex.go_to(sftp ? "sftp://srv" : "ftps://srv") && dialog.showing() && dialog.server().equals("srv") && dialog.password_box().password());
      dialog.user_box().set_text("alice");
      dialog.password_box().set_text("secret");
      click_center(w, dialog.ok_button());
      CHECK(!dialog.showing() && record.calls == 1 && record.server == "srv" && record.user == "alice" && record.password == "secret");
      CHECK(ex.path().equals(sftp ? "/SFTP/srv" : "/FTPS/srv") && ex.view().item_count() == 1);
      fs.unlocked = false;
      fs.failure = filesystem::Status::UntrustedHost;
      CHECK(!ex.go_to(sftp ? "sftp://srv/" : "ftps://srv/") && !dialog.showing() && record.calls == 1);   // (an untrusted server is not a reason to ask for a login)
      mgr.remove_window(&ex);
    }
  }
  CHECK(a.live == 0);
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
    // New Folder / New File: unique names, the new item is selected
    {
      filesystem::DirectoryEntry made;
      CHECK(ex.new_folder() && fs.stat("/New Folder", &made) == filesystem::Status::Ok && made.type == filesystem::NodeType::Directory);
      CHECK(ex.new_folder() && fs.stat("/New Folder 2", &made) == filesystem::Status::Ok);
      CHECK(ex.new_file() && fs.stat("/New File.txt", &made) == filesystem::Status::Ok && made.type == filesystem::NodeType::File && made.size == 0);
      CHECK(ex.new_file() && fs.stat("/New File 2.txt", &made) == filesystem::Status::Ok);
      CHECK(ex.view().item_count() == 9 && ex.view().selected_item() >= 0);
      // The new item's name is open for editing; typing a name and pressing Enter renames it.
      CHECK(ex.view().renaming());
      ex.view().rename_box().set_text("Projects");
      press(w, key::kEnter);
      CHECK(!ex.view().renaming() && fs.stat("/Projects", &made) == filesystem::Status::Ok && fs.stat("/New File 2.txt", &made) == filesystem::Status::NotFound);
      CHECK(ex.view().selected_item() >= 0);
      // Escape drops an edit; a taken or empty name is refused and nothing changes.
      CHECK(ex.rename_selected() && ex.view().renaming());
      ex.view().rename_box().set_text("zzz");
      press(w, key::kEscape);
      CHECK(!ex.view().renaming() && fs.stat("/zzz", &made) == filesystem::Status::NotFound);
      CHECK(!ex.rename_item("Projects", "New Folder 2") && !ex.rename_item("Projects", "") && !ex.rename_item("Projects", "a/b"));
      CHECK(ex.rename_item("Projects", "Projects") && ex.rename_item("Projects", "  Work  ") && fs.stat("/Work", &made) == filesystem::Status::Ok);
      CHECK(fs.rename("/Work", "/New File 2.txt") == filesystem::Status::Ok);
      // Delete asks first: Cancel keeps the item, Delete removes a folder with its contents.
      CHECK(fs.create_directory("/New Folder/inner") == filesystem::Status::Ok);
      make_file(fs, "/New Folder/inner/f.txt", "x");
      CHECK(ex.refresh());
      for (uint32_t i = 0; i < ex.view().item_count(); ++i) { String t(a); ex.view().item_text(i, 0, &t); if (t.view().equals("New Folder")) ex.view().select_item(static_cast<int32_t>(i)); }
      CHECK(ex.delete_selected() && ex.delete_dialog().showing());
      ex.delete_dialog().decline_button().click();
      CHECK(!ex.delete_dialog().showing() && fs.stat("/New Folder", &made) == filesystem::Status::Ok);
      CHECK(ex.delete_selected());
      ex.delete_dialog().accept_button().click();
      CHECK(!ex.delete_dialog().showing() && fs.stat("/New Folder", &made) == filesystem::Status::NotFound);
      CHECK(fs.create_directory("/New Folder") == filesystem::Status::Ok);
      CHECK(ex.refresh());
      CHECK(fs.remove("/New Folder") == filesystem::Status::Ok && fs.remove("/New Folder 2") == filesystem::Status::Ok);
      CHECK(fs.remove("/New File.txt") == filesystem::Status::Ok && fs.remove("/New File 2.txt") == filesystem::Status::Ok);
      CHECK(ex.refresh() && ex.view().item_count() == 5);
    }
    // File > Eject: the selected folder at the root, else the folder of the path's first component; the answer goes to the status bar
    {
      EjectProbe probe;
      ex.set_eject_handler(probe_eject, &probe);
      CHECK(ex.open_folder("/") && !ex.eject_selected() && probe.calls == 0 && ex.status_label().text().equals("Select a drive to eject"));
      ex.view().select_item(0);   // a folder (Apps)
      CHECK(ex.eject_selected() && probe.calls == 1 && strcmp(probe.volume, "Apps") == 0 && ex.status_label().text().equals("ejected"));
      CHECK(ex.open_folder("/Apps") && ex.eject_selected() && probe.calls == 2 && strcmp(probe.volume, "Apps") == 0);
      probe.answer = nullptr;
      CHECK(ex.eject_selected() && probe.calls == 3);
      ex.set_eject_handler(nullptr, nullptr);
      CHECK(!ex.eject_selected() && ex.status_label().text().equals("Nothing here can be ejected") && probe.calls == 3);
      ex.open_folder("/");
    }
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
  int32_t cpu_temp = 62500;
  desktop::TaskGpu gpu_info;
  int32_t cpu_temperature_millicelsius() override { return cpu_temp; }
  desktop::TaskGpu gpu() override { return gpu_info; }
};

static void test_computer_manager() {
  TestAllocator a;
  {
    FakeMonitor mon;
    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::ComputerManager tm(a);
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
    CHECK(tm.tabs().page_count() == 3 && tm.tabs().page_title(0).equals("Processes") && tm.tabs().page_title(1).equals("Performance") && tm.tabs().page_title(2).equals("Hardware") && tm.tabs().selected_page() == 0);

    // The GPU, its memory and the temperatures: unknown figures read n/a, known ones fill their meters.
    CHECK(tm.gpu_label().text().equals("n/a") && tm.vram_label().text().equals("n/a") && tm.gpu_temp_label().text().equals("n/a"));
    CHECK(tm.cpu_temp_label().text().equals("62.5 C") && tm.cpu_temp_meter().level() == 62);
    mon.gpu_info.usage_percent = 40;
    mon.gpu_info.memory_known = true;
    mon.gpu_info.memory_total_bytes = 1ull << 30;
    mon.gpu_info.memory_used_bytes = 256ull << 20;
    mon.gpu_info.temperature_millicelsius = 71000;
    tm.update(1000000);
    CHECK(tm.gpu_label().text().equals("40%") && tm.gpu_meter().level() == 40);
    CHECK(tm.vram_label().text().equals("256.0 MB / 1.0 GB") && tm.vram_meter().level() == 25);
    CHECK(tm.gpu_temp_label().text().equals("71.0 C") && tm.gpu_temp_meter().level() == 71);
    mon.gpu_info.memory_total_bytes = 0;     // a virtual GPU: used is known, the total is not
    tm.update(1000000);
    CHECK(tm.vram_label().text().equals("256.0 MB used") && tm.vram_meter().level() == 0);
    mon.gpu_info = desktop::TaskGpu();

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

// --- Tree view and the computer manager's Hardware tab ------------------------------------------------------------

static Point node_point(TreeView& t, int32_t node, int32_t dx = 80) {
  Rect2D r;
  CHECK(t.node_rect(node, &r));
  const Point o = t.window_origin();
  return {o.x + r.x + dx, o.y + r.y + static_cast<int32_t>(r.height) / 2};
}

static void test_tree_view() {
  TestAllocator a;
  {
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    TreeView t(a);
    t.set_dock(DockStyle::Fill);
    w.add(&t);
    w.layout();
    Counter selected;
    t.on_selection_changed(selected.handler());
    CHECK(t.node_count() == 0 && t.selected() == -1 && t.visible_count() == 0 && t.add_node(5, "x") == -1);
    // A small tree: two groups, the first with three devices, one of them with a child.
    const int32_t g1 = t.add_node(-1, "Group one", 100, ItemIcon::Folder);
    const int32_t g2 = t.add_node(-1, "Group two", 200, ItemIcon::Folder);
    const int32_t a1 = t.add_node(g1, "Alpha", 1), b1 = t.add_node(g1, "Beta", 2), c1 = t.add_node(g1, "Gamma", 3);
    const int32_t d1 = t.add_node(g2, "Delta", 4);
    const int32_t sub = t.add_node(b1, "Beta child", 5);
    CHECK(g1 == 0 && g2 == 1 && t.node_count() == 7 && t.child_count(-1) == 2 && t.child_count(g1) == 3 && t.child_count(b1) == 1 && t.child_count(a1) == 0);
    CHECK(t.child_at(-1, 1) == g2 && t.child_at(g1, 2) == c1 && t.child_at(g1, 3) == -1 && t.child_at(99, 0) == -1);
    CHECK(t.parent_of(sub) == b1 && t.parent_of(g1) == -1 && t.node_tag(d1) == 4 && t.node_text(c1).equals("Gamma"));
    // Everything is closed: only the two groups show.
    CHECK(t.visible_count() == 2 && t.node_at_row(0) == g1 && t.node_at_row(1) == g2 && !t.expanded(g1));
    Rect2D r;
    CHECK(t.node_rect(g1, &r) && !t.node_rect(a1, &r) && !t.box_rect(a1, &r));
    // The box opens a group; a click on the row selects it.
    Rect2D box;
    CHECK(t.box_rect(g1, &box));
    const Point o = t.window_origin();
    click(w, o.x + box.x + 3, o.y + box.y + 3);
    CHECK(t.expanded(g1) && t.visible_count() == 5 && t.selected() == -1);
    const Point at_beta = node_point(t, b1, 90);
    click(w, at_beta.x, at_beta.y);
    CHECK(t.selected() == b1 && selected.count == 1 && !t.expanded(b1));
    click(w, at_beta.x, at_beta.y);
    CHECK(selected.count == 1);   // (the same node: no change)

    // Keys: Right opens, Right again goes to the first child, Left goes back to the parent, Left closes, Up and Down walk the shown nodes.
    w.set_focus(&t);
    press(w, hardware::key::kRight);
    CHECK(t.expanded(b1) && t.selected() == b1 && t.visible_count() == 6);
    press(w, hardware::key::kRight);
    CHECK(t.selected() == sub);
    press(w, hardware::key::kLeft);
    CHECK(t.selected() == b1);
    press(w, hardware::key::kLeft);
    CHECK(!t.expanded(b1) && t.selected() == b1 && t.visible_count() == 5);
    press(w, hardware::key::kDown);
    CHECK(t.selected() == c1);
    press(w, hardware::key::kDown);
    CHECK(t.selected() == g2);
    press(w, hardware::key::kDown);
    CHECK(t.selected() == g2);   // (the last shown node)
    press(w, hardware::key::kEnter);
    CHECK(t.expanded(g2) && t.visible_count() == 6);
    press(w, 0x4A);   // Home
    CHECK(t.selected() == g1);
    press(w, 0x4D);   // End
    CHECK(t.selected() == d1);
    press(w, hardware::key::kSpace);
    CHECK(t.selected() == d1);   // (no children: nothing to open)
    press(w, hardware::key::kUp);
    CHECK(t.selected() == g2);
    // Closing a group that holds the selection hands it to the group.
    t.select(a1);
    CHECK(t.selected() == a1);
    t.set_expanded(g1, false);
    CHECK(t.selected() == g1 && t.visible_count() == 3);
    // select() opens the parents of a node that is hidden.
    t.select(sub);
    CHECK(t.expanded(g1) && t.expanded(b1) && t.selected() == sub);
    CHECK(t.node_at(5, 5) == g1 && t.node_at(5, 295) == -1);

    // The menu belongs to the nodes marked for it: a right click elsewhere opens nothing.
    ContextMenu menu(a);
    Counter chosen;
    menu.add_item("Do it", chosen.handler());
    t.set_context_menu(&menu);
    t.set_node_menu(g2, &menu);
    CHECK(!t.accepts_context_menu(-1, -1) || t.selected() == g2);
    t.select(g1);
    const Point on_g1 = node_point(t, g1), on_g2 = node_point(t, g2);
    CHECK(!choose_context(w, menu, on_g1.x, on_g1.y, "Do it") && !w.popup());
    CHECK(choose_context(w, menu, on_g2.x, on_g2.y, "Do it") && chosen.count == 1 && t.selected() == g2);   // (the right click selects it too)
    // clear() empties it.
    t.clear();
    CHECK(t.node_count() == 0 && t.visible_count() == 0 && t.selected() == -1);

    // A tall tree scrolls: the wheel, the scroll bar, and the keys bring the selection into view.
    for (int i = 0; i < 40; ++i) {
      String name(a);
      name.assign("Node ");
      name.append_uint(static_cast<uint64_t>(i));
      t.add_node(-1, name.view(), static_cast<uint32_t>(i));
    }
    w.layout();
    CHECK(t.visible_count() == 40);
    CHECK(t.node_rect(0, &r) && !t.node_rect(39, &r));
    w.mouse_move(100, 100);
    w.mouse_wheel(100, 100, -1);
    CHECK(!t.node_rect(0, &r) && t.node_rect(3, &r));
    t.select(39);
    CHECK(t.node_rect(39, &r) && !t.node_rect(0, &r));
    press(w, 0x4B);   // Page Up
    CHECK(t.selected() < 39 && t.node_rect(t.selected(), &r));
    w.render();
  }
  CHECK(a.live == 0);
}

struct FakeInventory final : desktop::IHardwareInventory {
  desktop::HardwareList list;
  bool changed = true;
  explicit FakeInventory(allocators::IAllocator& a) : list(a) {}
  bool refresh() override { const bool c = changed; changed = false; return c; }
  uint32_t count() const override { return list.count(); }
  bool entry(uint32_t i, desktop::HardwareEntry* out) const override {
    if (i >= list.count()) return false;
    *out = list.at(i);
    return true;
  }
};

static void test_hardware_tab() {
  TestAllocator a;
  {
    using desktop::HardwareGroup;
    // The list type itself.
    desktop::HardwareList one(a), two(a);
    CHECK(one.add(HardwareGroup::Storage, "Disk", "10 GB") && two.add(HardwareGroup::Storage, "Disk", "10 GB") && one.same_as(two));
    two.add(HardwareGroup::Audio, "Speaker");
    CHECK(!one.same_as(two) && two.count() == 2 && two.at(1).group == HardwareGroup::Audio);
    String longer(a);
    for (int i = 0; i < 200; ++i) longer.push_back('x');
    one.add(HardwareGroup::System, longer.view(), longer.view());
    CHECK(one.at(1).name[63] == 0 && one.at(1).detail[95] == 0 && strlen(one.at(1).name) == 63);
    CHECK(strcmp(desktop::hardware_group_name(HardwareGroup::Displays), "Displays") == 0 && strcmp(desktop::hardware_group_name(HardwareGroup::DisplayAdapters), "Display adapters") == 0);

    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::ComputerManager tm(a);
    FakeInventory inv(a);
    inv.list.add(HardwareGroup::Processors, "Fake CPU", "Fake CPU, 4 cores");
    inv.list.add(HardwareGroup::Memory, "2.0 GB of memory", "1.9 GB in the allocator pool");
    inv.list.add(HardwareGroup::Displays, "Monitor A", "HDMI-1, 1920x1080, primary");
    inv.list.add(HardwareGroup::Displays, "Monitor B", "HDMI-2, 1280x1024");
    inv.list.add(HardwareGroup::Storage, "Disk X", "8.0 GB");
    inv.list.add(HardwareGroup::Network, "Fake NIC one", "connected, 1 Gbps", 7);
    inv.list.add(HardwareGroup::Network, "Fake NIC two", "no link", 8);
    Counter opened, properties;
    tm.on_open_display_settings(opened.handler());
    tm.on_open_network_properties(properties.handler());
    CHECK(mgr.add_window(&tm));
    tm.set_hardware_inventory(&inv);
    tm.tabs().select_page(2);
    w.layout();
    TreeView& tree = tm.hardware_tree();
    // The groups that have devices, in a fixed order, all open at first; Network has none.
    CHECK(tree.child_count(-1) == 5 && tm.hardware_group_node(HardwareGroup::Network) >= 0 && tm.hardware_group_node(HardwareGroup::Audio) < 0);
    const int32_t displays = tm.hardware_group_node(HardwareGroup::Displays), storage = tm.hardware_group_node(HardwareGroup::Storage);
    CHECK(displays >= 0 && tree.node_text(displays).equals("Displays") && tree.child_count(displays) == 2 && tree.expanded(displays));
    CHECK(tree.node_text(tree.child_at(displays, 1)).equals("Monitor B") && tree.visible_count() == 5 + 1 + 1 + 2 + 1 + 2);
    CHECK(tm.tabs().selected_page() == 2 && tm.tabs().page_title(2).equals("Hardware") && tree.visible());
    // The line under the tree tells about the selected device or group.
    tree.select(tree.child_at(displays, 0));
    CHECK(tm.hardware_info().text().equals("HDMI-1, 1920x1080, primary"));
    tree.select(displays);
    CHECK(tm.hardware_info().text().find(StringView("2 devices")) != StringView::npos && tm.hardware_info().text().find(StringView("Display Settings")) != StringView::npos);
    tree.select(storage);
    CHECK(tm.hardware_info().text().equals("1 device"));
    // Display Settings is in the menu of the Displays group, and of nothing else.
    const Point on_displays = node_point(tree, displays), on_storage = node_point(tree, storage);
    CHECK(!choose_context(w, tm.display_menu(), on_storage.x, on_storage.y, "Display Settings...") && !w.popup() && opened.count == 0);
    CHECK(choose_context(w, tm.display_menu(), on_displays.x, on_displays.y, "Display Settings...") && opened.count == 1);
    const Point on_monitor = node_point(tree, tree.child_at(displays, 0));
    right_click(w, on_monitor.x, on_monitor.y);
    CHECK(!w.popup() && opened.count == 1);
    // A network adapter has Properties (and only that); the group of adapters has no menu; the Displays menu still works afterwards.
    const int32_t network = tm.hardware_group_node(HardwareGroup::Network);
    const int32_t nic = tree.child_at(network, 1);
    CHECK(tree.node_text(nic).equals("Fake NIC two") && tree.child_count(network) == 2);
    const Point on_nic = node_point(tree, nic), on_adapters = node_point(tree, network);
    CHECK(choose_context(w, tm.adapter_menu(), on_nic.x, on_nic.y, "Properties...") && properties.count == 1 && tm.network_adapter() == 8 && opened.count == 1);
    const int32_t first_nic = tree.child_at(network, 0);
    const Point on_first = node_point(tree, first_nic);
    CHECK(choose_context(w, tm.adapter_menu(), on_first.x, on_first.y, "Properties...") && properties.count == 2 && tm.network_adapter() == 7);
    right_click(w, on_adapters.x, on_adapters.y);
    CHECK(!w.popup() && properties.count == 2);
    CHECK(choose_context(w, tm.display_menu(), on_displays.x, on_displays.y, "Display Settings...") && opened.count == 2);

    // A new look at the machine: another monitor, a network adapter; what was closed stays closed and the selection stays on its device.
    tree.set_expanded(storage, false);
    tree.select(tree.child_at(displays, 1));
    inv.list.add(HardwareGroup::Audio, "Fake speaker", "48 kHz");
    inv.changed = true;
    tm.update(1000000);
    CHECK(tree.child_count(-1) == 6 && tm.hardware_group_node(HardwareGroup::Audio) >= 0);
    CHECK(!tree.expanded(tm.hardware_group_node(HardwareGroup::Storage)) && tree.expanded(tm.hardware_group_node(HardwareGroup::Displays)));
    CHECK(tree.selected() >= 0 && tree.node_text(tree.selected()).equals("Monitor B") && tm.hardware_info().text().equals("HDMI-2, 1280x1024"));
    // Nothing changed: the tree is left alone (a user who scrolled or selected keeps it).
    const uint32_t nodes = tree.node_count();
    tm.update(2000000);
    CHECK(tree.node_count() == nodes && tree.node_text(tree.selected()).equals("Monitor B"));
    // The View menu has the tab, checked when it is up.
    CHECK(choose(w, tm.menu_bar(), "View", "Processes") && tm.tabs().selected_page() == 0);
    CHECK(choose(w, tm.menu_bar(), "View", "Hardware") && tm.tabs().selected_page() == 2);
    // Without an inventory the tab says so.
    tm.set_hardware_inventory(nullptr);
    CHECK(tree.node_count() == 0 && tm.hardware_info().text().find(StringView("No hardware")) != StringView::npos);
    w.render();
    mgr.remove_window(&tm);
  }
  CHECK(a.live == 0);
}

struct FakeNetworkDetails final : desktop::INetworkDetails {
  desktop::NetworkAdapterInfo info;
  uint32_t known = 3;
  int reads = 0;
  bool adapter(uint32_t id, desktop::NetworkAdapterInfo* out) override {
    ++reads;
    if (id != known) return false;
    *out = info;
    return true;
  }
  static void set_ip(uint8_t* o, uint8_t a, uint8_t b, uint8_t c, uint8_t d) { o[0] = a; o[1] = b; o[2] = c; o[3] = d; }
  FakeNetworkDetails() {
    strcpy(info.name, "Fake e1000");
    const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0xAB};
    memcpy(info.mac, mac, 6);
    info.link_up = true;
    info.speed_mbps = 1000;
    info.duplex = 2;
    info.has_stack = info.configured = info.dhcp = true;
    strcpy(info.dhcp_state, "bound");
    info.lease_seconds = 86400;
    set_ip(info.address, 10, 0, 2, 15);
    set_ip(info.netmask, 255, 255, 255, 0);
    set_ip(info.gateway, 10, 0, 2, 2);
    set_ip(info.dns, 10, 0, 2, 3);
    set_ip(info.dhcp_server, 10, 0, 2, 2);
    info.frames_sent = 120;
    info.frames_received = 340;
  }
};

// The network adapter properties: link state, speed, and the IP, gateway and DNS the adapter was given.
static void test_network_properties() {
  TestAllocator a;
  {
    using P = desktop::NetworkProperties;
    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    P props(a);
    FakeNetworkDetails details;
    CHECK(mgr.add_window(&props));
    props.update();   // (no source yet)
    CHECK(props.cell(P::kLink, 1).empty());
    props.set_details(&details);
    props.show_adapter(3);
    w.layout();
    CHECK(props.adapter() == 3 && props.title().equals("Fake e1000 - Properties") && props.grid().row_count() == P::kRowCount);
    CHECK(props.cell(P::kLink, 0).equals("Link") && props.cell(P::kLink, 1).equals("Connected"));
    CHECK(props.cell(P::kSpeed, 1).equals("1 Gbps, full duplex") && props.cell(P::kMac, 1).equals("52:54:00:12:34:AB"));
    CHECK(props.cell(P::kConfiguration, 1).equals("Automatic (DHCP, bound)"));
    CHECK(props.cell(P::kAddress, 1).equals("10.0.2.15") && props.cell(P::kMask, 1).equals("255.255.255.0") && props.cell(P::kGateway, 1).equals("10.0.2.2") && props.cell(P::kDns, 1).equals("10.0.2.3"));
    CHECK(props.cell(P::kDhcpServer, 1).equals("10.0.2.2") && props.cell(P::kLease, 1).equals("24 h 0 min"));
    CHECK(props.cell(P::kSent, 1).equals("120") && props.cell(P::kReceived, 1).equals("340") && props.cell(P::kDropped, 1).equals("0"));
    // The link goes down, at 100 Mbps half duplex it comes back, and a manual configuration has no lease.
    details.info.link_up = false;
    props.update();
    CHECK(props.cell(P::kLink, 1).equals("Disconnected") && props.cell(P::kSpeed, 1).equals("-"));
    details.info.link_up = true;
    details.info.speed_mbps = 100;
    details.info.duplex = 1;
    details.info.dhcp = false;
    props.update();
    CHECK(props.cell(P::kSpeed, 1).equals("100 Mbps, half duplex") && props.cell(P::kConfiguration, 1).equals("Manual") && props.cell(P::kLease, 1).equals("-") && props.cell(P::kDhcpServer, 1).equals("-"));
    details.info.speed_mbps = 0;
    details.info.duplex = 0;
    props.update();
    CHECK(props.cell(P::kSpeed, 1).equals("Not reported by the adapter"));
    details.info.speed_mbps = 2500;
    props.update();
    CHECK(props.cell(P::kSpeed, 1).equals("2.5 Gbps") || props.cell(P::kSpeed, 1).equals("2500 Mbps"));
    // No address yet (DHCP selecting) and no stack at all.
    details.info.configured = false;
    details.info.dhcp = true;
    strcpy(details.info.dhcp_state, "selecting");
    memset(details.info.address, 0, 4);
    memset(details.info.gateway, 0, 4);
    props.update();
    CHECK(props.cell(P::kAddress, 1).equals("Not assigned") && props.cell(P::kGateway, 1).equals("Not assigned") && props.cell(P::kConfiguration, 1).equals("Automatic (DHCP, selecting)"));
    details.info.has_stack = false;
    props.update();
    CHECK(props.cell(P::kConfiguration, 1).equals("No network stack on this adapter") && props.cell(P::kDns, 1).equals("Not assigned"));
    // An adapter that is gone.
    details.known = 9;
    props.update();
    CHECK(props.cell(P::kLink, 1).empty());
    details.known = 3;
    props.update();
    CHECK(props.cell(P::kLink, 1).equals("Connected"));
    // Copy Row: the selected line goes to the clipboard.
    kernel::Clipboard clipboard(a);
    w.set_clipboard(&clipboard);
    props.grid().select_row(P::kMac);
    click_center(w, props.copy_button());
    String copied(a);
    CHECK(clipboard.get_text(&copied) && copied.view().find(StringView("Hardware address")) != StringView::npos && copied.view().find(StringView("52:54")) != StringView::npos);
    // Close.
    props.close();
    CHECK(!props.manager());
  }
  CHECK(a.live == 0);
}

// Display Settings is no longer in the Start menu: it is in the menu of the Displays group of the computer manager's hardware tree.
static void test_display_settings_entry() {
  TestAllocator a;
  {
    using desktop::HardwareGroup;
    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    CHECK(d.start_button().menu().item_count() == 9);   // Explorer, Computer Manager, Text Editor, Terminal, Paint, Media Player, About, a line, Shut Down
    FakeInventory inv(a);
    inv.list.add(HardwareGroup::Displays, "Monitor A", "HDMI-1");
    d.set_hardware_inventory(&inv);
    d.open_computer_manager();
    desktop::ComputerManager& cm = d.computer_manager();
    cm.tabs().select_page(2);
    d.window().layout();
    Window& w = d.window();
    const int32_t displays = cm.hardware_group_node(HardwareGroup::Displays);
    CHECK(displays >= 0 && !d.display_settings().manager());
    const Point p = node_point(cm.hardware_tree(), displays);
    CHECK(choose_context(w, cm.display_menu(), p.x, p.y, "Display Settings...") && d.display_settings().manager() != nullptr);
    CHECK(strcmp(cm.title().data(), "Computer Manager") == 0);

    // A network adapter's Properties opens its window, which follows the adapter once a second. (The display settings window is out of the way.)
    d.display_settings().close();
    cm.activate();
    inv.list.add(HardwareGroup::Network, "Fake e1000", "connected", 3);
    inv.changed = true;
    FakeNetworkDetails details;
    d.set_network_details(&details);
    cm.update(1000000);
    const int32_t network = cm.hardware_group_node(HardwareGroup::Network);
    CHECK(network >= 0 && !d.network_properties().manager());
    const Point q = node_point(cm.hardware_tree(), cm.hardware_tree().child_at(network, 0));
    CHECK(choose_context(w, cm.adapter_menu(), q.x, q.y, "Properties...") && d.network_properties().manager() != nullptr);
    CHECK(d.network_properties().adapter() == 3 && d.network_properties().cell(desktop::NetworkProperties::kAddress, 1).equals("10.0.2.15"));
    details.info.link_up = false;
    d.step(5000000);
    CHECK(d.network_properties().cell(desktop::NetworkProperties::kLink, 1).equals("Disconnected"));
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

static void test_media_player() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    make_file(fs, "/note.txt", "this is not media at all, just words");
    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    gui::NoticeDialog notices(a);
    desktop::MediaPlayer mp(a);
    mp.set_notices(&notices);
    CHECK(mgr.add_window(&mp));
    w.layout();
    CHECK(mp.title().equals("Media Player") && mp.kind() == desktop::MediaPlayer::Kind::None);
    CHECK(desktop::MediaPlayer::is_media_name("Song.MP3") && desktop::MediaPlayer::is_media_name("clip.mp4") && !desktop::MediaPlayer::is_media_name("notes.txt"));
    // Without storage, or with a file that is not media, the player says so in the notice dialog.
    CHECK(!mp.open_file("/note.txt") && notices.showing() && notices.kind() == gui::NoticeKind::Error);
    notices.close();
    mp.set_file_system(&fs);
    CHECK(!mp.open_file("/missing.wav") && notices.showing() && notices.message().first(11).equals("Cannot open"));
    notices.close();
    CHECK(!mp.open_file("/note.txt") && notices.showing() && mp.kind() == desktop::MediaPlayer::Kind::None);
    notices.close();
    mgr.remove_window(&mp);
    // With a sound mixer: sound files of two kinds are played from the file, a piece at a time.
    {
      FakeDevice device;
      audio::AudioMixer mixer(device, a);
      CHECK(mixer.initialize());
      desktop::MediaPlayer sound_player(a);
      sound_player.set_notices(&notices);
      sound_player.set_services(&mixer, nullptr, nullptr);
      sound_player.set_file_system(&fs);
      CHECK(mgr.add_window(&sound_player));
      w.layout();
      media::AudioData tone(a);
      audio::StreamFormat format;
      format.sample_rate = 8000;
      format.channels = 1;
      CHECK(tone.create(format, 16000));
      for (uint64_t i = 0; i < 16000; ++i) tone.samples()[i] = static_cast<int16_t>((i % 80) * 300 - 12000);
      ByteBuffer wav(a), flac(a);
      CHECK(media::encode_wav(tone, &wav) && media::encode_flac(tone, &flac));
      const struct { const char* path; ConstByteView bytes; } files[2] = {{"/tone.wav", wav.view()}, {"/tone.flac", flac.view()}};
      for (const auto& f : files) {
        UniquePointer<filesystem::IFile> file;
        uint64_t written = 0;
        CHECK(fs.open_file(f.path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok && file->write(0, f.bytes, &written) == filesystem::Status::Ok);
        file.reset();
        CHECK(sound_player.open_file(f.path) && sound_player.kind() == desktop::MediaPlayer::Kind::Audio && !notices.showing());
        CHECK(sound_player.audio_player()->duration() == 16000 && sound_player.audio_player()->playing());
        for (int i = 0; i < 20; ++i) {
          sound_player.audio_player()->update();
          mixer.pump();
          device.play_all();
        }
        CHECK(sound_player.audio_player()->position() > 1000);
        sound_player.close_media();
        CHECK(sound_player.kind() == desktop::MediaPlayer::Kind::None && sound_player.audio_player()->state() == audio::PlayerState::Empty);
      }
      // A MIDI file: played by the software synthesizer, or on the MIDI hardware when the machine has some.
      {
        static const uint8_t kSong[] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k', 0, 0, 0, 13,
                                        0x00, 0x90, 0x3C, 0x64, 0x83, 0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00};
        UniquePointer<filesystem::IFile> file;
        uint64_t written = 0;
        CHECK(fs.open_file("/song.mid", filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok &&
              file->write(0, ConstByteView(kSong, sizeof kSong), &written) == filesystem::Status::Ok);
        file.reset();
        CHECK(desktop::MediaPlayer::is_media_name("tune.MID") && desktop::MediaPlayer::is_media_name("tune.midi") && desktop::MediaPlayer::is_media_name("x.kar"));
        CHECK(sound_player.open_file("/song.mid") && sound_player.kind() == desktop::MediaPlayer::Kind::Audio && !notices.showing());
        CHECK(sound_player.status_label().text().equals("Playing /song.mid (synthesized)") && sound_player.audio_player()->duration() >= 48000);
        int loud = 0;
        for (int i = 0; i < 40; ++i) {
          sound_player.audio_player()->update();
          mixer.pump();
          device.play_all();
        }
        CHECK(sound_player.audio_player()->position() > 1000);
        sound_player.close_media();
        struct Sink final : midi::IMidiOutput {
          bool available() const override { return true; }
          int notes = 0, silences = 0;
          void send(uint8_t status, uint8_t d1, uint8_t) override {
            if ((status & 0xF0) == 0x90) ++notes;
            if ((status & 0xF0) == 0xB0 && d1 == 123) ++silences;
          }
          void reset() override {}
        } hardware;
        sound_player.set_midi_output(&hardware);
        CHECK(sound_player.open_file("/song.mid") && sound_player.status_label().text().equals("Playing /song.mid (on the MIDI hardware)"));
        for (int i = 0; i < 40; ++i) {
          sound_player.audio_player()->update();
          mixer.pump();
          device.play_all();
        }
        CHECK(hardware.notes == 1);
        sound_player.close_media();
        CHECK(hardware.silences >= 16);
        sound_player.set_midi_output(nullptr);
        (void)loud;
      }
      mgr.remove_window(&sound_player);
      mixer.pump();
    }
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
    // With a notice dialog the error opens in it instead of going to the status bar.
    {
      gui::NoticeDialog notices(a);
      np.set_notices(&notices);
      CHECK(!np.open_file("/gone") && notices.showing() && notices.kind() == gui::NoticeKind::Error && notices.message().equals("Cannot open /gone: not found"));
      notices.close();
      np.set_notices(nullptr);
    }

    // Unsaved changes are only thrown away when the command is repeated.
    w.set_focus(&np.editor());
    np.editor().set_caret(np.editor().text().size());
    type_char(w, 'c');
    CHECK(np.editor().modified() && np.title().equals("Text Editor - t.txt *") && np.position_label().text().equals("Ln 3, Col 2"));
    CHECK(!np.new_document() && np.status_label().text().first(13).equals("Unsaved chang"));
    type_char(w, 'd');                          // typing again disarms it
    CHECK(!np.new_document() && np.editor().text().equals("a\nb\ncd"));
    CHECK(np.new_document() && np.editor().text().empty() && np.path().empty() && np.title().equals("Text Editor - Untitled"));

    // Saving writes the text to the file it came from; a new document has none (and without a dialog nothing asks).
    type_text(w, "hello");
    press(w, key::kEnter);
    type_text(w, "world");
    press(w, key::kA + 18, modifier::kLeftControl);
    CHECK(np.editor().modified() && np.status_label().text().equals("Enter a path to save to."));
    CHECK(np.save_file("out.txt"));
    CHECK(!np.editor().modified() && np.path().equals("/out.txt") && np.status_label().text().equals("Saved /out.txt") && np.title().equals("Text Editor - out.txt"));
    UniquePointer<filesystem::IFile> f;
    CHECK(fs.open_file("/out.txt", filesystem::OpenMode::Read, &f) == filesystem::Status::Ok && f->size() == 11);
    char got[16] = {};
    uint64_t n = 0;
    CHECK(f->read(0, ByteView(reinterpret_cast<uint8_t*>(got), 11), &n) == filesystem::Status::Ok && !strcmp(got, "hello\nworld"));
    f.reset();
    type_text(w, "!");
    press(w, key::kA + 18, modifier::kLeftControl);   // Ctrl+S writes to the same file again
    CHECK(!np.editor().modified() && np.status_label().text().equals("Saved /out.txt"));
    CHECK(!np.save_file("/missing/x.txt") && np.status_label().text().equals("Cannot save /missing/x.txt: not found"));
    CHECK(np.path().equals("/out.txt"));
    press(w, key::kA + 14, modifier::kLeftControl);   // Ctrl+O without a dialog
    CHECK(np.status_label().text().equals("No file dialog available."));
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
    desktop::FileDialog dlg(a);
    dlg.set_file_system(&fs);

    // --- Text editor
    {
      desktop::Notepad np(a);
      np.set_file_system(&fs);
      np.set_file_dialog(&dlg);
      CHECK(mgr.add_window(&np));
      w.layout();
      MenuBar& bar = np.menu_bar();
      CHECK(bar.menu_count() == 2 && bar.menu(0)->title().equals("File") && bar.menu(1)->title().equals("Edit") && bar.parent() == &np);
      CHECK(bar.bounds().width > 100 && bar.window_origin().y < np.editor().window_origin().y && bar.find_item("File", "Save As..."));
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
      // File > Save with no file yet opens the Save dialog; the name is completed with the type's extension.
      CHECK(choose(w, bar, "File", "Save") && dlg.showing() && dlg.mode() == desktop::FileDialog::Mode::Save && dlg.name_box().text().empty());
      dlg.name_box().set_text("note");
      dlg.ok_button().click();
      CHECK(!dlg.showing() && dlg.accepted() && np.status_label().text().equals("Saved /note.txt") && !np.editor().modified() && np.path().equals("/note.txt"));
      CHECK(choose(w, bar, "File", "New") && np.editor().text().empty() && np.path().empty());
      // File > Open... shows the Open dialog; picking a file loads it.
      CHECK(choose(w, bar, "File", "Open...") && dlg.showing() && dlg.mode() == desktop::FileDialog::Mode::Open);
      dlg.name_box().set_text("a.txt");
      dlg.ok_button().click();
      CHECK(!dlg.showing() && np.editor().text().equals("abc") && np.path().equals("/a.txt"));
      press(w, key::kA + 18, modifier::kLeftControl);    // Ctrl+S writes to the same file
      CHECK(np.status_label().text().equals("Saved /a.txt"));
      CHECK(choose(w, bar, "File", "Save As...") && dlg.showing() && dlg.name_box().text().equals("a.txt") && dlg.folder().equals("/"));
      dlg.cancel_button().click();                       // cancelled: nothing changes
      CHECK(!dlg.showing() && !dlg.accepted() && np.path().equals("/a.txt"));
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
      desktop::ComputerManager tm(a);
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
      paint.set_file_dialog(&dlg);
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
      CHECK(choose(w, bar, "File", "Save") && dlg.showing());     // no file yet: the Save dialog
      dlg.name_box().set_text("pic");
      dlg.ok_button().click();
      CHECK(paint.path().equals("/pic.png") && paint.title().equals("Paint - pic.png") && !c.modified());
      paint.tools().select_tool(0);
      drag(w, pixel_point(c, 50, 50), pixel_point(c, 60, 50));
      CHECK(choose(w, bar, "File", "New") && paint.status_label().text().first(13).equals("Unsaved chang") && c.pixel(55, 50) != 0xFFFFFFFF);
      CHECK(choose(w, bar, "File", "New") && c.pixel(55, 50) == 0xFFFFFFFF && c.image_width() == 640);
      CHECK(choose(w, bar, "File", "Open...") && dlg.showing() && dlg.list().item_count() > 0);
      dlg.name_box().set_text("pic.png");
      dlg.ok_button().click();
      CHECK(!dlg.showing() && paint.path().equals("/pic.png") && c.pixel(25, 10) == 0xFF000000);
      CHECK(choose(w, bar, "File", "Close") && paint.manager() == nullptr);
    }
    w.render();
  }
  CHECK(a.live == 0);
}

// --- File dialogs ------------------------------------------------------------------------------------------------

struct Closed2 {
  int count = 0;
  static void fn(void* context, Control&) { ++static_cast<Closed2*>(context)->count; }
  Handler handler() { return {&Closed2::fn, this}; }
};

static void test_file_dialog() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    CHECK(fs.create_directory("/docs") == filesystem::Status::Ok);
    CHECK(fs.create_directory("/docs/old") == filesystem::Status::Ok);
    make_file(fs, "/docs/note.md", "# hi");
    make_file(fs, "/a.txt", "abc");
    make_file(fs, "/b.png", "x");
    make_file(fs, "/c.TXT", "upper");
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    desktop::FileDialog dlg(a);
    using Mode = desktop::FileDialog::Mode;
    static const desktop::FileDialog::Filter kFilters[] = {{"Text files", "txt;md"}, {"Pictures", "png"}, {"All files", "*"}};
    Closed2 closed;
    auto names = [&](uint32_t i) {
      String s(a);
      dlg.list().item_text(i, 0, &s);
      return s;
    };

    // No filesystem: nothing opens.
    CHECK(!dlg.show(mgr, Mode::Open, "Open", kFilters, 3, "/", closed.handler()) && !dlg.showing());
    dlg.set_file_system(&fs);

    // The Open dialog: folders first, then the files of the chosen type.
    CHECK(dlg.show(mgr, Mode::Open, "Open", kFilters, 3, "/", closed.handler()) && dlg.showing() && mgr.has_modal());
    CHECK(!dlg.show(mgr, Mode::Open, "Open", kFilters, 3, "/", closed.handler()));   // already showing
    w.layout();
    CHECK(dlg.title().equals("Open") && dlg.ok_button().window() == &w && w.focus() == &dlg.name_box());
    CHECK(dlg.folder().equals("/") && dlg.address_bar().path().equals("/") && dlg.type_box().item_count() == 3 && dlg.type_box().selected_index() == 0);
    CHECK(dlg.list().item_count() == 3 && names(0).view().equals("docs") && names(1).view().equals("a.txt") && names(2).view().equals("c.TXT"));
    dlg.type_box().set_selected_index(1);        // another type: pictures
    CHECK(dlg.list().item_count() == 2 && names(1).view().equals("b.png"));
    dlg.type_box().set_selected_index(2);
    CHECK(dlg.list().item_count() == 4);
    dlg.type_box().set_selected_index(0);

    // A click on a file puts its name in the box; Enter on a folder goes into it.
    dlg.list().select_item(1);
    CHECK(dlg.name_box().text().equals("a.txt"));
    dlg.list().select_item(0);                   // a folder: the name box is left alone
    CHECK(dlg.name_box().text().equals("a.txt"));
    w.set_focus(&dlg.list());
    press(w, key::kEnter);
    CHECK(dlg.folder().equals("/docs") && dlg.address_bar().path().equals("/docs") && dlg.list().item_count() == 2 && names(0).view().equals("old") && names(1).view().equals("note.md"));
    press(w, key::kBackspace);                   // Backspace in the list: up one folder
    CHECK(dlg.folder().equals("/"));
    {
      const Point bar = dlg.address_bar().window_origin();   // the breadcrumb works too
      dlg.list().select_item(0);
      press(w, key::kEnter);
      CHECK(dlg.folder().equals("/docs"));
      click(w, bar.x + dlg.address_bar().segment(0).x + 4, bar.y + 12);
      CHECK(dlg.folder().equals("/"));
    }

    // Names that do not exist or are not picked are refused with a message; a folder name goes into the folder.
    dlg.name_box().set_text("zzz.txt");
    dlg.ok_button().click();
    CHECK(dlg.showing() && dlg.message_label().text().equals("Not found: zzz.txt") && closed.count == 0);
    dlg.name_box().set_text("");
    dlg.ok_button().click();
    CHECK(dlg.message_label().text().equals("Enter a file name."));
    dlg.name_box().set_text("docs");
    dlg.ok_button().click();
    CHECK(dlg.folder().equals("/docs") && dlg.name_box().text().empty() && dlg.message_label().text().empty());
    dlg.address_bar().set_path("/");
    dlg.open_folder("/");

    // A file name typed with a path, accepted with Enter in the box.
    dlg.name_box().set_text("/docs/note.md");
    w.set_focus(&dlg.name_box());
    press(w, key::kEnter);
    CHECK(!dlg.showing() && dlg.accepted() && dlg.selected_path().equals("/docs/note.md") && closed.count == 1 && !mgr.has_modal());

    // Cancel, Escape and the close button leave nothing picked.
    CHECK(dlg.show(mgr, Mode::Open, "Open", kFilters, 3, "/a.txt", closed.handler()));
    CHECK(dlg.name_box().text().equals("a.txt") && dlg.folder().equals("/"));   // started on that file
    dlg.cancel_button().click();
    CHECK(!dlg.showing() && !dlg.accepted() && closed.count == 2);
    CHECK(dlg.show(mgr, Mode::Open, "Open", kFilters, 3, "/docs", closed.handler()) && dlg.folder().equals("/docs") && dlg.name_box().text().empty());
    w.set_focus(&dlg.list());
    press(w, key::kEscape);
    CHECK(!dlg.showing() && !dlg.accepted() && closed.count == 3);
    CHECK(dlg.show(mgr, Mode::Open, "Open", kFilters, 3, "/nowhere/x.txt", closed.handler()) && dlg.folder().equals("/") && dlg.name_box().text().equals("x.txt"));
    dlg.close();
    CHECK(!dlg.showing() && !dlg.accepted());

    // Double-clicking a file (with a clock) takes it.
    uint64_t now = 1000000;
    dlg.list().set_clock(&now);
    CHECK(dlg.show(mgr, Mode::Open, "Open", kFilters, 3, "/", closed.handler()));
    w.layout();
    {
      Rect2D row;
      CHECK(dlg.list().item_rect(1, &row));
      const Point o = dlg.list().window_origin();
      click(w, o.x + row.x + 30, o.y + row.y + 5);
      now += 100000;
      click(w, o.x + row.x + 30, o.y + row.y + 5);
    }
    CHECK(!dlg.showing() && dlg.accepted() && dlg.selected_path().equals("/a.txt"));
    dlg.list().set_clock(nullptr);

    // The Save dialog: the type's first extension is added to a bare name; a name that is a file asks before replacing it.
    CHECK(dlg.show(mgr, Mode::Save, "Save As", kFilters, 3, "/docs", closed.handler()) && dlg.ok_button().text().equals("Save"));
    dlg.name_box().set_text("fresh");
    dlg.ok_button().click();
    CHECK(!dlg.showing() && dlg.accepted() && dlg.selected_path().equals("/docs/fresh.txt"));
    CHECK(dlg.show(mgr, Mode::Save, "Save As", kFilters, 3, "/a.txt", closed.handler()));
    CHECK(dlg.type_box().selected_index() == 0 && dlg.name_box().text().equals("a.txt"));
    dlg.ok_button().click();                     // a.txt exists
    CHECK(dlg.showing() && !dlg.accepted() && dlg.replace_dialog().showing());
    CHECK(dlg.replace_dialog().decline_button().text().equals("Cancel"));
    dlg.replace_dialog().decline_button().click();
    CHECK(!dlg.replace_dialog().showing() && dlg.showing() && !dlg.accepted() && w.focus() == &dlg.name_box());
    dlg.ok_button().click();
    dlg.replace_dialog().accept_button().click();   // Replace
    CHECK(!dlg.replace_dialog().showing() && !dlg.showing() && dlg.accepted() && dlg.selected_path().equals("/a.txt"));
    // Another type is chosen by name: .png for a bare name, and the files shown are those of the type.
    CHECK(dlg.show(mgr, Mode::Save, "Save As", kFilters, 3, "/", closed.handler()));
    dlg.type_box().set_selected_index(1);
    CHECK(dlg.list().item_count() == 2);
    dlg.name_box().set_text("shot");
    dlg.ok_button().click();
    CHECK(dlg.accepted() && dlg.selected_path().equals("/shot.png"));
    // A name with its own extension keeps it, and an existing file is found with the case it has.
    CHECK(dlg.show(mgr, Mode::Save, "Save As", kFilters, 3, "/", closed.handler()));
    dlg.name_box().set_text("data.bin");
    dlg.ok_button().click();
    CHECK(dlg.accepted() && dlg.selected_path().equals("/data.bin"));
    // The folder has to exist.
    CHECK(dlg.show(mgr, Mode::Save, "Save As", kFilters, 3, "/", closed.handler()));
    dlg.name_box().set_text("/missing/file.txt");
    dlg.ok_button().click();
    CHECK(dlg.showing() && dlg.message_label().text().equals("The folder does not exist."));
    dlg.cancel_button().click();
    // Starting on a file picks the type that fits it; a typed address goes there.
    CHECK(dlg.show(mgr, Mode::Open, "Open", kFilters, 3, "/b.png", closed.handler()) && dlg.type_box().selected_index() == 1);
    dlg.address_bar().begin_edit();
    dlg.address_bar().edit_box().set_text("/docs/old");
    press(w, key::kEnter);
    CHECK(dlg.folder().equals("/docs/old") && dlg.showing());
    dlg.address_bar().begin_edit();
    dlg.address_bar().edit_box().set_text("/nowhere");
    press(w, key::kEnter);
    CHECK(dlg.folder().equals("/docs/old") && dlg.message_label().text().equals("That folder cannot be opened."));
    w.render();
    dlg.cancel_button().click();
    // With no filters given every file is shown.
    CHECK(dlg.show(mgr, Mode::Open, "Open", nullptr, 0, "/", closed.handler()) && dlg.type_box().item_count() == 1 && dlg.list().item_count() == 4);
    dlg.cancel_button().click();
    CHECK(!mgr.has_modal());
  }
  CHECK(a.live == 0);
}

// The desktop gives the text editor and Paint one shared file dialog.
static void test_desktop_file_dialog() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    make_file(fs, "/hello.txt", "hi there");
    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.set_file_system(&fs);
    d.open_text_editor();
    d.open_paint();
    d.window().layout();
    CHECK(choose(d.window(), d.notepad().menu_bar(), "File", "Open..."));
    CHECK(d.file_dialog().showing() && d.windows().has_modal());
    d.file_dialog().name_box().set_text("hello.txt");
    d.file_dialog().ok_button().click();
    CHECK(!d.file_dialog().showing() && d.notepad().editor().text().equals("hi there") && d.notepad().path().equals("/hello.txt"));
    // Double-clicking a folder in the dialog goes into it, and double-clicking a file takes it (the desktop supplies the clock).
    CHECK(fs.create_directory("/sub") == filesystem::Status::Ok);
    make_file(fs, "/sub/inner.txt", "x");
    d.step(1000000);
    CHECK(choose(d.window(), d.notepad().menu_bar(), "File", "Open...") && d.file_dialog().folder().equals("/"));
    d.window().layout();
    {
      Rect2D row;
      CHECK(d.file_dialog().list().item_rect(0, &row));   // "sub", the only folder
      const Point o = d.file_dialog().list().window_origin();
      click(d.window(), o.x + row.x + 30, o.y + row.y + 5);
      CHECK(d.file_dialog().folder().equals("/"));        // one click only selects it
      d.step(1100000);
      click(d.window(), o.x + row.x + 30, o.y + row.y + 5);
      CHECK(d.file_dialog().folder().equals("/sub") && d.file_dialog().list().item_count() == 1);
      d.step(5000000);                                    // clicks too far apart are not a double click
      CHECK(d.file_dialog().list().item_rect(0, &row));
      click(d.window(), o.x + row.x + 30, o.y + row.y + 5);
      d.step(5900000);
      click(d.window(), o.x + row.x + 30, o.y + row.y + 5);
      CHECK(d.file_dialog().showing() && d.file_dialog().folder().equals("/sub"));
      d.step(6000000);
      click(d.window(), o.x + row.x + 30, o.y + row.y + 5);
      CHECK(!d.file_dialog().showing() && d.notepad().path().equals("/sub/inner.txt"));
    }
    // The print and color dialogs are shared the same way.
    CHECK(choose(d.window(), d.notepad().menu_bar(), "File", "Print...") && d.print_dialog().showing() && d.print_dialog().kind() == desktop::PrintDialog::Kind::Text);
    d.print_dialog().cancel_button().click();
    CHECK(choose(d.window(), d.paint().menu_bar(), "File", "Print...") && d.print_dialog().showing());
    d.print_dialog().cancel_button().click();
    CHECK(choose(d.window(), d.paint().menu_bar(), "Options", "Primary Color...") && d.color_dialog().showing());
    d.color_dialog().cancel_button().click();
  }
  CHECK(a.live == 0);
}

// --- Spin box, color spectrum and the color and print dialogs ---------------------------------------------------------

static void test_spin_box() {
  TestAllocator a;
  {
    Screen screen(a, 300, 200);
    Theme theme;
    Window w(a, screen.renderer, theme, {300, 200});
    SpinBox box(a);
    TextBox other(a);
    box.set_bounds({10, 10, 100, 24});
    other.set_bounds({10, 60, 100, 24});
    w.add(&box);
    w.add(&other);
    w.layout();
    Counter changes;
    box.on_value_changed(changes.handler());
    box.set_range(1, 20);
    changes.count = 0;
    box.set_value(5);
    CHECK(box.value() == 5 && box.edit_box().text().equals("5") && changes.count == 1);
    box.set_value(5);
    CHECK(changes.count == 1);                           // unchanged: no event
    box.set_value(99);
    CHECK(box.value() == 20 && box.edit_box().text().equals("20"));
    box.set_value(-4);
    CHECK(box.value() == 1);

    // The two buttons step.
    click(w, 10 + box.up_rect().x + 4, 10 + box.up_rect().y + 4);
    CHECK(box.value() == 2);
    click(w, 10 + box.down_rect().x + 4, 10 + box.down_rect().y + 4);
    click(w, 10 + box.down_rect().x + 4, 10 + box.down_rect().y + 4);
    CHECK(box.value() == 1);                             // not below the minimum
    box.set_step(3);
    click(w, 10 + box.up_rect().x + 4, 10 + box.up_rect().y + 4);
    CHECK(box.value() == 4);
    box.set_step(1);

    // Keys and the wheel; Page Up goes ten steps.
    w.set_focus(&box.edit_box());
    press(w, key::kUp);
    CHECK(box.value() == 5);
    press(w, key::kDown);
    press(w, key::kDown);
    CHECK(box.value() == 3);
    press(w, 0x4B);
    CHECK(box.value() == 13);
    press(w, 0x4B);
    CHECK(box.value() == 20);
    w.mouse_wheel(20, 20, -1);
    CHECK(box.value() == 19);

    // Typing a number: it counts as soon as it makes sense, and is corrected when the box is left.
    press(w, key::kA, modifier::kLeftControl);
    type_char(w, '1');
    type_char(w, '2');
    CHECK(box.edit_box().text().equals("12") && box.value() == 12);
    type_char(w, 'x');                                   // not part of a number: thrown out
    CHECK(box.edit_box().text().equals("12") && box.value() == 12);
    type_char(w, '5');
    CHECK(box.value() == 20 && box.edit_box().text().equals("125"));   // over the maximum: the value stops at it ...
    w.set_focus(&other);
    CHECK(box.edit_box().text().equals("20"));           // ... and the box says so once it is left
    w.set_focus(&box.edit_box());
    press(w, key::kA, modifier::kLeftControl);
    press(w, key::kBackspace);
    CHECK(box.value() == 20 && box.edit_box().text().empty());     // empty: the value waits
    press(w, key::kEnter);
    CHECK(box.edit_box().text().equals("20"));

    // Negative numbers when the range has some.
    box.set_range(-10, 10);
    box.set_value(-3);
    CHECK(box.edit_box().text().equals("-3"));
    press(w, key::kA, modifier::kLeftControl);
    type_char(w, '-');
    type_char(w, '7');
    CHECK(box.value() == -7);
    box.set_range(5, 8);                                 // a new range moves the value into it
    CHECK(box.value() == 5 && box.minimum() == 5 && box.maximum() == 8);
    box.set_range(9, 3);
    CHECK(box.maximum() == 9 && box.value() == 9);
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_color_spectrum() {
  TestAllocator a;
  {
    Screen screen(a, 300, 250);
    Theme theme;
    Window w(a, screen.renderer, theme, {300, 250});
    ColorSpectrum sp(a);
    sp.set_bounds({10, 10, 200, 170});
    w.add(&sp);
    w.layout();
    Counter changes;
    sp.on_color_changed(changes.handler());
    CHECK(sp.hsv().hue == 0 && sp.hsv().saturation == 255 && sp.hsv().value == 255 && sp.color() == 0xFFFF0000);
    sp.set_color(0xFF00FF00);
    CHECK(sp.hsv().hue == 120 && sp.color() == 0xFF00FF00 && changes.count == 0);   // from code: no event
    // Clicking the square picks saturation (across) and brightness (down).
    click(w, 10 + 0, 10 + 0);                                 // top left: no saturation, full brightness = white
    CHECK(sp.hsv().saturation == 0 && sp.hsv().value == 255 && sp.color() == 0xFFFFFFFF && changes.count == 1 && sp.hsv().hue == 120);
    click(w, 10 + ColorSpectrum::kField - 1, 10 + ColorSpectrum::kField - 1);   // bottom right: black
    CHECK(sp.hsv().value == 0 && sp.hsv().saturation == 255 && sp.color() == 0xFF000000);
    click(w, 10 + ColorSpectrum::kField - 1, 10);             // top right: the pure hue
    CHECK(sp.color() == 0xFF00FF00 && changes.count == 3);
    // The strip picks the hue, keeping the other two; a drag follows the pointer and may leave the control.
    const Rect2D strip = sp.strip_rect();
    click(w, 10 + strip.x + 4, 10 + ColorSpectrum::kField - 1);
    CHECK(sp.hsv().hue == 359 && sp.hsv().saturation == 255 && sp.hsv().value == 255);
    w.mouse_move(10 + strip.x + 4, 10 + 80);
    w.mouse_button(10 + strip.x + 4, 10 + 80, MouseButton::Left, true);
    w.mouse_move(10 + strip.x + 40, 10 + 0);
    w.mouse_move(10 + strip.x + 40, 10 - 50);
    w.mouse_button(10 + strip.x + 40, 10 - 50, MouseButton::Left, false);
    CHECK(sp.hsv().hue == 0);
    CHECK(sp.field_marker().x == ColorSpectrum::kField - 1 && sp.field_marker().y == 0 && sp.strip_marker() == 0);
    // Keys.
    w.set_focus(&sp);
    press(w, key::kLeft);
    CHECK(sp.hsv().saturation == 247);
    press(w, key::kDown);
    CHECK(sp.hsv().value == 247);
    press(w, 0x4B);
    CHECK(sp.hsv().hue == 10);
    press(w, 0x4E);
    press(w, 0x4E);
    CHECK(sp.hsv().hue == 350);
    for (int i = 0; i < 40; ++i) press(w, key::kUp);
    CHECK(sp.hsv().value == 255);
    w.render();
    sp.set_hsv(Hsv{200, 100, 100});
    w.render();                                               // another hue: the square is drawn again
  }
  CHECK(a.live == 0);
}

static void test_color_dialog() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    desktop::ColorDialog dlg(a);
    Closed2 closed;
    CHECK(dlg.show(mgr, "Pick", 0xFF336699, closed.handler()) && dlg.showing() && mgr.has_modal() && !dlg.show(mgr, "Pick", 0xFF000000, closed.handler()));
    w.layout();
    // It starts at the color it was given, which every part shows.
    CHECK(dlg.title().equals("Pick") && dlg.color() == 0xFF336699 && dlg.preview_color() == 0xFF336699 && dlg.original_color() == 0xFF336699);
    CHECK(dlg.red_box().value() == 0x33 && dlg.green_box().value() == 0x66 && dlg.blue_box().value() == 0x99 && dlg.hex_box().text().equals("#336699"));
    CHECK(red_of(dlg.spectrum().color()) >= 0x30 && red_of(dlg.spectrum().color()) <= 0x36 && blue_of(dlg.spectrum().color()) >= 0x96);

    // Each way of choosing changes the others.
    dlg.red_box().set_value(255);
    CHECK(dlg.color() == 0xFFFF6699 && dlg.hex_box().text().equals("#FF6699") && dlg.preview_color() == 0xFFFF6699 && dlg.original_color() == 0xFF336699);
    CHECK(red_of(dlg.spectrum().color()) >= 0xFD);
    dlg.hex_box().set_text("00ff80");
    CHECK(dlg.color() == 0xFF00FF80 && dlg.red_box().value() == 0 && dlg.green_box().value() == 255 && dlg.blue_box().value() == 128);
    dlg.hex_box().set_text("#12");                       // not a color yet: nothing happens
    CHECK(dlg.color() == 0xFF00FF80);
    dlg.hex_box().set_text("#GG0000");
    CHECK(dlg.color() == 0xFF00FF80);
    const Point sp = dlg.spectrum().window_origin();
    click(w, sp.x + 0, sp.y + 0);                        // the white corner of the square
    CHECK(dlg.color() == 0xFFFFFFFF && dlg.red_box().value() == 255 && dlg.hex_box().text().equals("#FFFFFF"));
    const uint16_t hue = dlg.spectrum().hsv().hue;
    dlg.set_color(0xFF808080);                           // a grey keeps the hue the strip shows
    CHECK(dlg.color() == 0xFF808080 && dlg.spectrum().hsv().hue == hue);
    const Rect2D swatch = dlg.basic_colors().swatch_rect(16);   // red among the basic colors
    const Point bp = dlg.basic_colors().window_origin();
    click(w, bp.x + swatch.x + 3, bp.y + swatch.y + 3);
    CHECK(dlg.color() == 0xFFFF0000 && dlg.green_box().value() == 0 && dlg.hex_box().text().equals("#FF0000"));
    w.render();

    // OK takes the choice; the dialog started from the color it was given.
    dlg.ok_button().click();
    CHECK(!dlg.showing() && dlg.accepted() && dlg.color() == 0xFFFF0000 && closed.count == 1 && !mgr.has_modal());
    // Cancel, Escape and Enter.
    CHECK(dlg.show(mgr, "Pick", 0x00123456, closed.handler()) && dlg.color() == 0xFF123456 && dlg.original_color() == 0xFF123456);   // the alpha is ignored
    dlg.set_color(0xFFABCDEF);
    dlg.cancel_button().click();
    CHECK(!dlg.showing() && !dlg.accepted() && closed.count == 2);
    CHECK(dlg.show(mgr, "Pick", 0xFF000000, closed.handler()));
    w.set_focus(&dlg.hex_box());
    press(w, key::kEscape);
    CHECK(!dlg.showing() && !dlg.accepted() && closed.count == 3);
    CHECK(dlg.show(mgr, "Pick", 0xFF000000, closed.handler()));
    dlg.hex_box().set_text("#0A0B0C");
    w.set_focus(&dlg.hex_box());
    press(w, key::kEnter);
    CHECK(!dlg.showing() && dlg.accepted() && dlg.color() == 0xFF0A0B0C);
  }
  CHECK(a.live == 0);
}

static void test_print_preview() {
  TestAllocator a;
  {
    Screen screen(a, 400, 400);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 400});
    desktop::PrintPreview pv(a);
    pv.set_bounds({10, 10, 220, 300});
    w.add(&pv);
    w.layout();
    // Nothing to show: a blank sheet in the proportions of the paper, centred.
    pv.set_sheet(hardware::PaperSize::A4, false, 42);
    CHECK(pv.page_count() == 0);
    Rect2D sheet = pv.sheet_rect();
    CHECK(sheet.width > 100 && sheet.height > sheet.width && sheet.width <= 204 && std::abs(static_cast<int>(sheet.width * 297 / 210) - static_cast<int>(sheet.height)) <= 2);
    CHECK(sheet.x * 2 + static_cast<int32_t>(sheet.width) == 220 && sheet.y * 2 + static_cast<int32_t>(sheet.height) == 300);
    const Rect2D area = pv.printable_rect();
    CHECK(area.x > sheet.x && area.y > sheet.y && area.width < sheet.width && area.height < sheet.height && area.x - sheet.x == static_cast<int32_t>(42 * sheet.width / 2100));
    w.render();
    CHECK(screen.px(10 + sheet.x + 5, 10 + sheet.y + 5) == 0xFFFFFFFF);          // paper
    pv.set_sheet(hardware::PaperSize::A4, true, 42);                              // landscape: wider than high
    CHECK(pv.sheet_rect().width > pv.sheet_rect().height && pv.sheet_rect().width <= 204);
    pv.set_sheet(hardware::PaperSize::Letter, false, 42);
    CHECK(std::abs(static_cast<int>(pv.sheet_rect().width * 2794 / 2159) - static_cast<int>(pv.sheet_rect().height)) <= 2);
    pv.set_sheet(hardware::PaperSize::A4, false, 42);

    // A text: its pages, the lines that have something on them, and bars where the characters are.
    hardware::TextLayout layout(a);
    const hardware::TextGeometry g = hardware::text_geometry(hardware::PaperSize::A4, false, 42);
    CHECK(layout.layout("XXXXXXXXXX\n\n  XX XX\fsecond\n\nthird page\f", g.columns, g.lines) && layout.page_count() == 2);
    pv.show_text(&layout);
    CHECK(pv.page_count() == 2 && pv.page() == 0 && pv.drawn_lines(0) == 2 && pv.drawn_lines(1) == 2 && pv.drawn_lines(5) == 0);
    w.render();
    const Rect2D a0 = pv.printable_rect();
    const int32_t line_h = static_cast<int32_t>(a0.height / g.lines);
    CHECK(screen.px(10 + a0.x + 1, 10 + a0.y + line_h / 2) == 0xFF303030);       // the first line is a bar from the left margin
    CHECK(screen.px(10 + a0.x + static_cast<int32_t>(a0.width) - 3, 10 + a0.y + line_h / 2) == 0xFFFFFFFF);   // not all the way across
    CHECK(screen.px(10 + a0.x + 1, 10 + a0.y + line_h * 3 / 2 + line_h / 2) == 0xFFFFFFFF);   // the empty second line has none
    pv.set_page(1);
    CHECK(pv.page() == 1);
    w.render();
    CHECK(screen.px(10 + a0.x + 1, 10 + a0.y + line_h / 2) == 0xFF303030);       // page two starts with "second": a bar, shorter than the first page's
    CHECK(screen.px(10 + a0.x + static_cast<int32_t>(a0.width) * 9 / 10, 10 + a0.y + line_h / 2) == 0xFFFFFFFF);
    pv.set_page(9);
    CHECK(pv.page() == 1);                                                         // clamped to the pages there are
    pv.show_text(nullptr);
    CHECK(pv.page_count() == 0 && pv.page() == 0);

    // A picture: placed and halftoned like the print; black fills its part of the page.
    std::vector<uint32_t> black(40 * 20, 0xFF000000);
    graphics::Surface picture;
    picture.pixels = ArrayView<uint32_t>(black.data(), black.size());
    picture.width = 40;
    picture.height = 20;
    picture.pitch = 40;
    pv.show_picture(&picture, hardware::Fit::Contain, hardware::Halftone::Threshold);
    CHECK(pv.page_count() == 1 && pv.page() == 0);
    w.render();
    const Rect2D ar = pv.printable_rect();
    const int32_t cx = 10 + ar.x + static_cast<int32_t>(ar.width) / 2, cy = 10 + ar.y + static_cast<int32_t>(ar.height) / 2;
    CHECK(screen.px(cx, cy) == 0xFF202020);                                        // the middle of the picture
    CHECK(screen.px(cx, 10 + ar.y + 2) == 0xFFFFFFFF);                              // contained: the page above and below it stays white
    pv.show_picture(&picture, hardware::Fit::Stretch, hardware::Halftone::Threshold);
    w.render();
    CHECK(screen.px(cx, 10 + ar.y + 2) == 0xFF202020);                              // stretched: it fills the page
    pv.show_picture(&picture, hardware::Fit::Contain, hardware::Halftone::ErrorDiffusion);
    pv.set_bounds({10, 10, 120, 160});                                              // another size: drawn again
    w.layout();
    w.render();
    pv.show_nothing();
    CHECK(pv.page_count() == 0);
    w.render();
  }
  CHECK(a.live == 0);
}

struct FakePrintService final : desktop::IPrintService {
  struct Printer {
    const char* name;
    bool color, duplex, present;
    uint16_t papers, max_dpi;
    uint8_t margin = 42;
  };
  Printer printers[3] = {{"Office laser", false, true, true, 0x03, 1200}, {"Color jet", true, false, true, 0x01, 600}, {"Gone", false, false, false, 0x03, 600}};
  uint32_t count = 3;
  uint32_t last_printer = 99;
  std::string last_text, last_name;
  uint32_t picture_w = 0, picture_h = 0;
  desktop::PrintOptions last_options;
  int jobs = 0;
  const char* fail_with = nullptr;
  uint32_t printer_count() override { return count; }
  bool printer(uint32_t i, desktop::PrinterListing* out) override {
    if (i >= count) return false;
    *out = desktop::PrinterListing();
    strncpy(out->name, printers[i].name, sizeof out->name - 1);
    out->color = printers[i].color;
    out->duplex = printers[i].duplex;
    out->present = printers[i].present;
    out->paper_sizes = printers[i].papers;
    out->max_dpi = printers[i].max_dpi;
    out->unprintable_mm10 = printers[i].margin;
    out->pages_per_minute = 20;
    return true;
  }
  uint32_t default_printer() override { return 0; }
  bool print_text(uint32_t p, StringView text, StringView name, const desktop::PrintOptions& o, String* error) override {
    if (fail_with) { error->assign(fail_with); return false; }
    ++jobs;
    last_printer = p;
    last_text.assign(text.data(), text.size());
    last_name.assign(name.data(), name.size());
    last_options = o;
    picture_w = picture_h = 0;
    return true;
  }
  bool print_picture(uint32_t p, const graphics::Surface& pic, StringView name, const desktop::PrintOptions& o, String* error) override {
    if (fail_with) { error->assign(fail_with); return false; }
    ++jobs;
    last_printer = p;
    last_name.assign(name.data(), name.size());
    last_options = o;
    last_text.clear();
    picture_w = pic.width;
    picture_h = pic.height;
    return true;
  }
};

static void test_print_dialog() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    w.layout();
    desktop::PrintDialog dlg(a);
    using Kind = desktop::PrintDialog::Kind;
    Closed2 closed;
    FakePrintService service;

    // Without a service there are no printers, and nothing can be printed.
    CHECK(dlg.show(mgr, Kind::Text, "Print", closed.handler()) && dlg.status_label().text().equals("No printers found.") && !dlg.print_button().enabled());
    dlg.print_button().click();
    CHECK(dlg.showing());
    dlg.cancel_button().click();
    CHECK(!dlg.showing() && !dlg.accepted() && closed.count == 1);
    dlg.set_print_service(&service);

    // The dialog lists the printers and shows the first (the default) with what it can do.
    CHECK(dlg.show(mgr, Kind::Text, "Print", closed.handler()) && !dlg.show(mgr, Kind::Text, "Print", closed.handler()));
    w.layout();
    CHECK(dlg.printer_box().item_count() == 3 && dlg.printer_box().item(1).equals("Color jet") && dlg.printer_box().selected_index() == 0);
    CHECK(dlg.status_label().text().equals("Ready - black and white, duplex, up to 1200 dpi, 20 pages/min") && dlg.print_button().enabled());
    CHECK(dlg.paper_box().item_count() == 2 && dlg.paper_box().selected_text().equals("A4") && dlg.paper_box().item(1).equals("Letter"));
    CHECK(dlg.quality_box().item_count() == 3 && dlg.quality_box().selected_text().equals("600 dpi (normal)"));
    CHECK(dlg.copies_box().value() == 1 && dlg.portrait_button().checked() && !dlg.landscape_button().checked() && !dlg.toner_box().checked());
    CHECK(!dlg.picture_box().parent()->visible() && !dlg.stretch_box().parent()->visible());       // picture options are for pictures
    // Another printer offers other things.
    dlg.printer_box().set_selected_index(1);
    CHECK(dlg.status_label().text().first(5).equals("Ready") && dlg.status_label().text().find("color") != StringView::npos);
    CHECK(dlg.paper_box().item_count() == 1 && dlg.quality_box().item_count() == 2 && dlg.quality_box().selected_text().equals("600 dpi (normal)"));
    dlg.printer_box().set_selected_index(2);              // one that is not there cannot be printed on
    CHECK(dlg.status_label().text().first(13).equals("Not connected") && !dlg.print_button().enabled());
    dlg.printer_box().set_selected_index(0);
    CHECK(dlg.print_button().enabled());

    // Choices come back as options.
    dlg.paper_box().set_selected_index(1);
    dlg.landscape_button().set_checked(true);
    dlg.quality_box().set_selected_index(2);
    dlg.copies_box().set_value(3);
    dlg.toner_box().set_checked(true);
    w.render();
    dlg.print_button().click();
    CHECK(!dlg.showing() && dlg.accepted() && closed.count == 2 && dlg.printer_index() == 0 && dlg.kind() == Kind::Text);
    CHECK(dlg.options().paper == hardware::PaperSize::Letter && dlg.options().landscape && dlg.options().dpi == 1200 && dlg.options().copies == 3 && dlg.options().toner_save);

    // A picture has more to choose; Enter prints, Escape cancels, and a typed copy count is taken.
    CHECK(dlg.show(mgr, Kind::Picture, "Print Picture", closed.handler()) && dlg.title().equals("Print Picture"));
    w.layout();
    CHECK(dlg.picture_box().parent()->visible() && dlg.stretch_box().parent()->visible() && dlg.copies_box().value() == 1 && !dlg.landscape_button().checked() && !dlg.toner_box().checked());
    dlg.printer_box().set_selected_index(1);
    dlg.picture_box().set_selected_index(1);
    dlg.stretch_box().set_checked(true);
    w.set_focus(&dlg.copies_box().edit_box());
    press(w, key::kA, modifier::kLeftControl);
    type_char(w, '7');
    press(w, key::kEnter);
    CHECK(!dlg.showing() && dlg.accepted() && dlg.printer_index() == 1 && dlg.options().copies == 7 && dlg.options().halftone == hardware::Halftone::Threshold &&
          dlg.options().fit == hardware::Fit::Stretch && dlg.options().dpi == 600);
    CHECK(dlg.show(mgr, Kind::Picture, "Print", closed.handler()));
    dlg.picture_box().set_selected_index(2);
    dlg.print_button().click();
    CHECK(dlg.options().halftone == hardware::Halftone::Ordered && dlg.options().fit == hardware::Fit::Contain);
    CHECK(dlg.show(mgr, Kind::Text, "Print", closed.handler()));
    w.set_focus(&dlg.printer_box());
    press(w, key::kEscape);
    CHECK(!dlg.showing() && !dlg.accepted());

    // A text with pages: the dialog counts them, shows them one at a time, and asks which to print.
    {
      String long_text(a);
      for (int i = 0; i < 150; ++i) {
        long_text.append("line ");
        long_text.append_uint(static_cast<uint64_t>(i));
        long_text.append("\n");
      }
      dlg.set_text(long_text.view());
      CHECK(dlg.show(mgr, Kind::Text, "Print", closed.handler()));
      w.layout();
      CHECK(dlg.page_count() == 3 && dlg.preview().page_count() == 3 && dlg.page_label().text().equals("Page 1 of 3"));   // 68 lines of A4 to a page
      CHECK(dlg.all_pages_button().parent()->visible() && dlg.all_pages_button().checked() && !dlg.from_page_box().enabled() && !dlg.to_page_box().enabled());
      CHECK(dlg.from_page_box().maximum() == 3 && dlg.to_page_box().maximum() == 3 && dlg.to_page_box().value() == 3);
      CHECK(!dlg.previous_page_button().enabled() && dlg.next_page_button().enabled() && dlg.preview().drawn_lines(0) == 68);
      dlg.next_page_button().click();
      CHECK(dlg.preview().page() == 1 && dlg.page_label().text().equals("Page 2 of 3") && dlg.previous_page_button().enabled());
      dlg.next_page_button().click();
      CHECK(dlg.page_label().text().equals("Page 3 of 3") && !dlg.next_page_button().enabled() && dlg.preview().drawn_lines(2) == 150 - 136);
      dlg.next_page_button().click();                          // at the end already
      CHECK(dlg.preview().page() == 2);
      dlg.previous_page_button().click();
      CHECK(dlg.preview().page() == 1);
      w.render();

      // Paper and orientation change the pages: sideways pages hold fewer lines, so there are more of them.
      dlg.landscape_button().set_checked(true);
      CHECK(dlg.page_count() == 4 && dlg.to_page_box().maximum() == 4 && dlg.to_page_box().value() == 4 && dlg.preview().sheet_rect().width > dlg.preview().sheet_rect().height);
      CHECK(dlg.preview().page() == 1 || dlg.preview().page() < 4);
      dlg.portrait_button().set_checked(true);
      CHECK(dlg.page_count() == 3);
      dlg.paper_box().set_selected_index(1);                  // Letter: 11 inches tall
      CHECK(dlg.page_count() == 3 && std::abs(static_cast<int>(dlg.preview().sheet_rect().height * 2159) - static_cast<int>(dlg.preview().sheet_rect().width * 2794)) < 3 * 2794);

      // Choosing "From": the boxes wake up, keep in order, and the preview goes to the first page of the range.
      dlg.page_range_button().set_checked(true);
      CHECK(dlg.from_page_box().enabled() && dlg.to_page_box().enabled() && !dlg.all_pages_button().checked());
      dlg.from_page_box().set_value(2);
      CHECK(dlg.to_page_box().value() == 3 && dlg.preview().page() == 1);
      dlg.to_page_box().set_value(1);                         // a last page before the first pulls the first back
      CHECK(dlg.from_page_box().value() == 1 && dlg.to_page_box().value() == 1);
      dlg.from_page_box().set_value(2);
      dlg.to_page_box().set_value(2);
      dlg.print_button().click();
      CHECK(!dlg.showing() && dlg.accepted() && dlg.options().first_page == 2 && dlg.options().last_page == 2 && dlg.options().paper == hardware::PaperSize::Letter);

      // "All" prints everything, whatever the boxes say.
      CHECK(dlg.show(mgr, Kind::Text, "Print", closed.handler()));
      CHECK(dlg.all_pages_button().checked() && dlg.preview().page() == 0 && dlg.paper_box().selected_text().equals("A4"));
      dlg.page_range_button().set_checked(true);
      dlg.from_page_box().set_value(3);
      dlg.all_pages_button().set_checked(true);
      dlg.print_button().click();
      CHECK(dlg.options().first_page == 1 && dlg.options().last_page == 0);

      // The other printer's margins change the layout too (a bigger margin leaves fewer lines).
      String longer(a);
      for (int i = 0; i < 200; ++i) longer.append("x\n");
      dlg.set_text(longer.view());
      service.printers[1].margin = 200;
      CHECK(dlg.show(mgr, Kind::Text, "Print", closed.handler()));
      const uint32_t pages_before = dlg.page_count();
      CHECK(pages_before == 3);
      dlg.printer_box().set_selected_index(1);
      CHECK(dlg.page_count() > pages_before);
      dlg.cancel_button().click();
      service.printers[1].margin = 42;

      // Short text: one page, and the page buttons are off. Nothing to print: nothing to preview.
      dlg.set_text("hello\n");
      CHECK(dlg.show(mgr, Kind::Text, "Print", closed.handler()) && dlg.page_count() == 1 && dlg.page_label().text().equals("Page 1 of 1") &&
            !dlg.previous_page_button().enabled() && !dlg.next_page_button().enabled());
      dlg.cancel_button().click();
      dlg.set_text(StringView());
      CHECK(dlg.show(mgr, Kind::Text, "Print", closed.handler()) && dlg.page_count() == 0 && dlg.page_label().text().equals("Nothing to preview"));
      dlg.cancel_button().click();

      // A picture is one page: its row of page choices goes, the preview shows the picture, and its options follow the dialog.
      std::vector<uint32_t> px(30 * 30, 0xFF000000);
      graphics::Surface picture;
      picture.pixels = ArrayView<uint32_t>(px.data(), px.size());
      picture.width = picture.height = picture.pitch = 30;
      dlg.set_picture(&picture);
      CHECK(dlg.show(mgr, Kind::Picture, "Print Picture", closed.handler()) && dlg.page_count() == 1 && dlg.page_label().text().equals("The page"));
      w.layout();
      CHECK(!dlg.all_pages_button().parent()->visible() && !dlg.next_page_button().enabled());
      dlg.stretch_box().set_checked(true);
      dlg.picture_box().set_selected_index(2);
      dlg.landscape_button().set_checked(true);
      w.render();
      dlg.print_button().click();
      CHECK(dlg.options().first_page == 1 && dlg.options().last_page == 0 && dlg.options().landscape && dlg.options().fit == hardware::Fit::Stretch);
    }

    // All printers gone.
    service.count = 0;
    CHECK(dlg.show(mgr, Kind::Text, "Print", closed.handler()) && dlg.printer_box().item_count() == 0 && !dlg.print_button().enabled());
    dlg.cancel_button().click();
    CHECK(!mgr.has_modal());
  }
  CHECK(a.live == 0);
}

static void test_print_and_color_flows() {
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
    desktop::PrintDialog print(a);
    desktop::ColorDialog colors(a);
    FakePrintService service;
    print.set_print_service(&service);

    // --- Text editor: File > Print...
    {
      desktop::Notepad np(a);
      CHECK(mgr.add_window(&np));
      w.layout();
      MenuBar& bar = np.menu_bar();
      CHECK(bar.find_item("File", "Print...") && bar.find_item("File", "Print...")->shortcut.view().equals("Ctrl+P"));
      CHECK(choose(w, bar, "File", "Print...") && np.status_label().text().equals("No print dialog available."));
      np.set_print_dialog(&print);
      CHECK(choose(w, bar, "File", "Print...") && !print.showing() && np.status_label().text().equals("There is nothing to print."));
      w.set_focus(&np.editor());
      type_text(w, "print me");
      CHECK(choose(w, bar, "File", "Print...") && print.showing() && print.kind() == desktop::PrintDialog::Kind::Text);
      print.copies_box().set_value(2);
      print.printer_box().set_selected_index(1);
      print.print_button().click();
      CHECK(!print.showing() && service.jobs == 1 && service.last_printer == 1 && service.last_text == "print me" && service.last_name == "Untitled" &&
            service.last_options.copies == 2 && np.status_label().text().equals("Sent to the printer."));
      press(w, key::kA + 15, modifier::kLeftControl);            // Ctrl+P
      CHECK(print.showing());
      print.cancel_button().click();
      CHECK(service.jobs == 1);
      // The editor's text is what the dialog previews, and a page range is passed on with the job.
      np.editor().set_text("one\fpage two\fthree");
      CHECK(choose(w, bar, "File", "Print...") && print.page_count() == 3 && print.page_label().text().equals("Page 1 of 3") && print.preview().drawn_lines(1) == 1);
      print.page_range_button().set_checked(true);
      print.from_page_box().set_value(2);
      print.print_button().click();
      CHECK(service.jobs == 2 && service.last_options.first_page == 2 && service.last_options.last_page == 3 && service.last_text == "one\fpage two\fthree");
      service.jobs = 1;
      service.fail_with = "The printer's queue is full.";
      CHECK(choose(w, bar, "File", "Print..."));
      print.print_button().click();
      CHECK(np.status_label().text().equals("Cannot print: The printer's queue is full.") && service.jobs == 1);
      service.fail_with = nullptr;
      CHECK(choose(w, bar, "File", "Close"));
    }

    // --- Paint: File > Print... and Options > Primary / Secondary Color...
    {
      desktop::Paint paint(a);
      CHECK(mgr.add_window(&paint));
      w.layout();
      MenuBar& bar = paint.menu_bar();
      DrawingCanvas& c = paint.canvas();
      CHECK(choose(w, bar, "File", "Print...") && paint.status_label().text().equals("No print dialog available."));
      CHECK(choose(w, bar, "Options", "Primary Color...") && paint.status_label().text().equals("No color dialog available."));
      paint.set_print_dialog(&print);
      paint.set_color_dialog(&colors);
      CHECK(choose(w, bar, "File", "Print...") && print.showing() && print.kind() == desktop::PrintDialog::Kind::Picture);
      print.picture_box().set_selected_index(1);
      print.print_button().click();
      CHECK(!print.showing() && service.jobs == 2 && service.picture_w == 640 && service.picture_h == 480 && service.last_name == "Untitled" &&
            service.last_options.halftone == hardware::Halftone::Threshold && paint.status_label().text().equals("Sent to the printer."));
      // A picture still being placed is set down before it is printed.
      paint.tools().select_tool(7);
      c.select_rect(0, 0, 20, 20);
      drag(w, pixel_point(c, 5, 5), pixel_point(c, 25, 25));
      CHECK(c.floating());
      CHECK(choose(w, bar, "File", "Print..."));
      CHECK(!c.floating());
      print.cancel_button().click();
      // Colors: the dialog starts at the current color; OK sets it in the palette and the canvas.
      CHECK(choose(w, bar, "Options", "Primary Color...") && colors.showing() && colors.title().equals("Primary Color") && colors.color() == paint.palette().primary());
      colors.set_color(0xFF336699);
      colors.ok_button().click();
      CHECK(!colors.showing() && paint.palette().primary() == 0xFF336699 && c.primary() == 0xFF336699 && paint.color_picker().color() == 0xFF336699);
      CHECK(choose(w, bar, "Options", "Secondary Color...") && colors.title().equals("Secondary Color") && colors.color() == 0xFFFFFFFF);
      colors.set_color(0xFFFFEE00);
      colors.cancel_button().click();                          // cancelled: nothing changes
      CHECK(paint.palette().secondary() == 0xFFFFFFFF);
      CHECK(choose(w, bar, "Options", "Secondary Color..."));
      colors.hex_box().set_text("#FFEE00");
      colors.ok_button().click();
      CHECK(paint.palette().secondary() == 0xFFFFEE00 && c.secondary() == 0xFFFFEE00);
      // What is drawn uses the new colors.
      paint.tools().select_tool(0);
      drag(w, pixel_point(c, 50, 50), pixel_point(c, 60, 50));
      drag(w, pixel_point(c, 50, 52), pixel_point(c, 60, 52), MouseButton::Right);
      CHECK(c.pixel(55, 50) == 0xFF336699 && c.pixel(55, 52) == 0xFFFFEE00);
      press(w, key::kA + 15, modifier::kLeftControl);            // Ctrl+P
      CHECK(print.showing());
      print.cancel_button().click();
      CHECK(choose(w, bar, "File", "Close"));
    }
    w.render();
  }
  CHECK(a.live == 0);
}

// --- Context menus -------------------------------------------------------------------------------------------------

static void test_context_menu_control() {
  TestAllocator a;
  {
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    Panel panel(a);
    Button inner(a), plain(a);
    panel.set_bounds({10, 10, 380, 280});
    inner.set_bounds({20, 20, 100, 30});
    plain.set_bounds({20, 100, 100, 30});
    panel.add(&inner);
    panel.add(&plain);
    w.add(&panel);
    ContextMenu menu(a);
    Counter first, second, opening;
    MenuItem* one = menu.add_item("First", first.handler(), "Ctrl+1");
    menu.add_separator();
    MenuItem* two = menu.add_item("Second", second.handler());
    menu.on_opening(Handler::immediate([](void* c, Control&) { ++static_cast<Counter*>(c)->count; }, &opening));
    CHECK(menu.find_item("Second") == two && !menu.find_item("Third") && !menu.open());
    w.layout();

    // Without a context menu the right button is the control's own.
    right_click(w, 30, 30);
    CHECK(!w.popup() && opening.count == 0);
    panel.set_context_menu(&menu);
    CHECK(panel.context_menu() == &menu);

    // A right click opens the menu at the pointer - on the panel, or on a control inside it that has none.
    right_click(w, 200, 150);
    CHECK(menu.open() && w.popup() != nullptr && opening.count == 1 && w.popup()->bounds().x == 200 && w.popup()->bounds().y == 150);
    CHECK(w.popup()->bounds().width >= 100 && w.popup()->bounds().height > 20);
    w.render();
    CHECK(click_popup_item(w, &menu.menu(), "First") && first.count == 1 && !menu.open() && !w.popup());
    CHECK(choose_context(w, menu, 30, 30, "Second") && second.count == 1 && opening.count == 2);      // on the button inside
    // Releasing the button right where the menu opened does not choose an item; moving first does.
    right_click(w, 385, 285);                                // so near the corner that the menu is moved up and left, under the pointer
    CHECK(menu.open() && w.popup()->bounds().x + static_cast<int32_t>(w.popup()->bounds().width) <= 400 && first.count == 1 && second.count == 1);
    w.close_popup();
    CHECK(!menu.open());
    // A disabled item does nothing.
    one->set_enabled(false);
    CHECK(!choose_context(w, menu, 200, 150, "First") && first.count == 1 && !w.popup());
    one->set_enabled(true);
    // A click elsewhere closes it; a right click elsewhere opens it again there.
    right_click(w, 200, 150);
    CHECK(menu.open());
    right_click(w, 250, 250);
    CHECK(menu.open() && w.popup()->bounds().x <= 250 && w.popup()->bounds().x + static_cast<int32_t>(w.popup()->bounds().width) <= 400 && opening.count == 6);   // (every opening counts)
    click(w, 5, 5);
    CHECK(!menu.open() && !w.popup());
    // A control can keep the right button for itself in places (here with no menu around it to fall back on).
    struct Picky final : Control {
      explicit Picky(allocators::IAllocator& al) : Control(al) {}
      bool accepts_context_menu(int32_t x, int32_t) const override { return x < 50; }
      int downs = 0;
      void on_mouse_down(const MouseArgs& args) override { if (args.button == MouseButton::Right) ++downs; }
    } picky(a);
    picky.set_bounds({150, 100, 100, 30});
    panel.add(&picky);
    panel.set_context_menu(nullptr);
    picky.set_context_menu(&menu);
    right_click(w, 170, 125);                                // the left half: the menu
    CHECK(menu.open() && picky.downs == 0);
    w.close_popup();
    right_click(w, 230, 125);                                // the right half: the control gets the click itself
    CHECK(!menu.open() && picky.downs == 1);
    picky.set_context_menu(nullptr);
    panel.set_context_menu(&menu);
    right_click(w, 230, 125);                                // now the panel's menu is the one that applies there
    CHECK(menu.open() && picky.downs == 1);
    w.close_popup();
    w.render();

    // From the keyboard: the Menu key or Shift+F10 opens it under the focused control, the arrows and Enter choose.
    w.set_focus(&inner);
    const int32_t before = opening.count;
    press(w, 0x65);
    CHECK(menu.open() && opening.count == before + 1 && w.popup()->bounds().y >= 20 + 30);
    press(w, key::kDown);
    press(w, key::kEnter);                                   // the first item is highlighted already; Down goes to the second
    CHECK(!menu.open() && second.count == 2);
    press(w, key::kF1 + 9, modifier::kLeftShift);
    CHECK(menu.open());
    press(w, key::kEscape);
    CHECK(!menu.open());
    // Without a window there is nothing to open in; without items there is no menu.
    ContextMenu empty(a);
    plain.set_context_menu(&empty);
    right_click(w, 30, 110);
    CHECK(!empty.open());
    plain.set_context_menu(nullptr);
    panel.remove(&picky);
  }
  CHECK(a.live == 0);
}

static void test_text_context_menus() {
  TestAllocator a;
  {
    kernel::Clipboard cb(a);
    Screen screen(a, 400, 300);
    Theme theme;
    Window w(a, screen.renderer, theme, {400, 300});
    w.set_clipboard(&cb);
    TextBox box(a);
    TextEditor ed(a);
    box.set_bounds({10, 10, 200, 24});
    ed.set_bounds({10, 60, 300, 100});
    w.add(&box);
    w.add(&ed);
    w.layout();
    String got(a);
    auto enabled = [&](ContextMenu* m, const char* item) { return m->find_item(item) && m->find_item(item)->enabled(); };

    // A text box: the items are enabled for what it holds, and run.
    ContextMenu* menu = box.context_menu();
    CHECK(menu != nullptr && menu->menu().item_count() == 0);          // built when first wanted
    box.set_text("hello world");
    right_click(w, 20, 20);
    CHECK(menu->open() && w.focus() == &box && menu->menu().item_count() == 6);
    CHECK(!enabled(menu, "Cut") && !enabled(menu, "Copy") && !enabled(menu, "Delete") && !enabled(menu, "Paste") && enabled(menu, "Select All"));
    w.close_popup();
    CHECK(choose_context(w, *menu, 20, 20, "Select All") && box.selected_text().equals("hello world"));
    right_click(w, 20, 20);                                            // inside the selection: it stays
    CHECK(box.has_selection() && enabled(menu, "Cut") && enabled(menu, "Copy") && enabled(menu, "Delete") && !enabled(menu, "Paste"));
    w.close_popup();
    CHECK(choose_context(w, *menu, 20, 20, "Copy") && cb.get_text(&got) && got.view().equals("hello world") && box.text().equals("hello world"));
    press(w, 0x4A);                                                    // Home, then select "hello"
    for (int i = 0; i < 5; ++i) press(w, key::kRight, modifier::kLeftShift);
    right_click(w, 20, 20);                                            // inside the selection: it stays
    CHECK(box.selected_text().equals("hello"));
    w.close_popup();
    right_click(w, 10 + 4 + 8 * 8, 20);                                // outside it: the caret goes where the click is
    CHECK(!box.has_selection() && box.caret() == 8 && enabled(menu, "Paste"));
    w.close_popup();
    CHECK(choose_context(w, *menu, 10 + 4 + 8 * 8, 20, "Paste") && box.text().equals("hello wohello worldrld"));
    box.set_text("one two");
    press(w, key::kA, modifier::kLeftControl);
    CHECK(choose_context(w, *menu, 20, 20, "Cut") && box.text().empty() && cb.get_text(&got) && got.view().equals("one two"));
    box.set_text("abc");
    press(w, key::kA, modifier::kLeftControl);
    CHECK(choose_context(w, *menu, 20, 20, "Delete") && box.text().empty());
    box.set_text("fixed");
    box.set_read_only(true);
    press(w, key::kA, modifier::kLeftControl);
    right_click(w, 20, 20);
    CHECK(!enabled(menu, "Cut") && enabled(menu, "Copy") && !enabled(menu, "Delete") && !enabled(menu, "Paste"));
    w.close_popup();
    box.set_read_only(false);
    // Without a window clipboard, copy and paste still work inside the box.
    w.set_clipboard(nullptr);
    box.set_text("private");
    press(w, key::kA, modifier::kLeftControl);
    CHECK(choose_context(w, *menu, 20, 20, "Copy"));
    press(w, 0x4D);
    right_click(w, 20, 20);
    CHECK(enabled(menu, "Paste"));
    w.close_popup();
    CHECK(choose_context(w, *menu, 20, 20, "Paste") && box.text().equals("pprivaterivate"));   // at the caret the click set
    w.set_clipboard(&cb);
    // The keyboard: the Menu key opens it under the box.
    w.set_focus(&box);
    press(w, 0x65);
    CHECK(menu->open() && w.popup()->bounds().y >= 10 + 24);
    press(w, key::kEscape);
    CHECK(!menu->open());

    // The multi-line editor has the same menu.
    ContextMenu* emenu = ed.context_menu();
    CHECK(emenu != nullptr && emenu != menu);
    ed.set_text("alpha beta");
    CHECK(choose_context(w, *emenu, 20, 70, "Select All") && ed.has_selection());
    CHECK(choose_context(w, *emenu, 20, 70, "Cut") && ed.text().empty() && cb.get_text(&got) && got.view().equals("alpha beta"));
    right_click(w, 20, 70);
    CHECK(!enabled(emenu, "Cut") && !enabled(emenu, "Select All") && enabled(emenu, "Paste"));    // empty: nothing to select
    w.close_popup();
    CHECK(choose_context(w, *emenu, 20, 70, "Paste") && ed.text().equals("alpha beta"));
    ed.set_caret(0);
    right_click(w, 10 + 3 + 6 * 8, 70 + 4);                            // a click in the text moves the caret there
    CHECK(ed.caret() == 6 && !ed.has_selection());
    w.close_popup();
    ed.set_read_only(true);
    right_click(w, 20, 70);
    CHECK(!enabled(emenu, "Paste") && !enabled(emenu, "Delete"));
    w.close_popup();
    ed.set_read_only(false);
    w.render();
  }
  CHECK(a.live == 0);
}

static void test_application_context_menus() {
  TestAllocator a;
  {
    kernel::Clipboard cb(a);
    filesystem::RamFileSystem fs(a);
    CHECK(fs.create_directory("/docs") == filesystem::Status::Ok);
    make_file(fs, "/a.txt", "abc");
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    w.set_clipboard(&cb);
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    String got(a);
    auto enabled = [&](ContextMenu& m, const char* item) { return m.find_item(item) && m.find_item(item)->enabled(); };

    // --- File explorer: a right click selects the item under the pointer; the menu follows the selection and the view.
    {
      desktop::FileExplorer ex(a);
      ex.set_file_system(&fs);
      CHECK(mgr.add_window(&ex));
      w.layout();
      Rect2D row;
      const Point o = ex.view().window_origin();
      CHECK(ex.view().item_rect(1, &row));                        // a.txt
      const int32_t fx = o.x + row.x + 30, fy = o.y + row.y + 5;
      right_click(w, fx, fy);
      ContextMenu& menu = ex.list_menu();
      CHECK(menu.open() && ex.view().selected_item() == 1 && enabled(menu, "Open") && enabled(menu, "Copy Path") && !enabled(menu, "Up One Level"));
      CHECK(menu.find_item("Details")->checked() && !menu.find_item("List")->checked());
      w.close_popup();
      CHECK(choose_context(w, menu, fx, fy, "Copy Path") && cb.get_text(&got) && got.view().equals("/a.txt"));
      CHECK(choose_context(w, menu, fx, fy, "Icons") && ex.view().view_mode() == ViewMode::Icons && ex.icons_item()->checked());
      CHECK(ex.view().item_rect(0, &row));
      CHECK(choose_context(w, menu, o.x + row.x + 20, o.y + row.y + 20, "Open") && ex.path().equals("/docs"));   // the folder, by its icon
      CHECK(choose_context(w, menu, o.x + 200, o.y + 100, "Details") && ex.view().view_mode() == ViewMode::Details);
      right_click(w, o.x + 200, o.y + 100);                       // an empty place: nothing is selected, so nothing to open
      CHECK(ex.view().selected_item() == -1 && !enabled(menu, "Open") && !enabled(menu, "Copy Path") && enabled(menu, "Up One Level"));
      w.close_popup();
      CHECK(choose_context(w, menu, o.x + 200, o.y + 100, "Up One Level") && ex.path().equals("/"));
      CHECK(choose_context(w, menu, o.x + 200, o.y + 100, "Refresh") && ex.view().item_count() == 2);
      // The address bar's menu.
      const Point bar = ex.address_bar().window_origin();
      CHECK(choose_context(w, ex.address_menu(), bar.x + 6, bar.y + 12, "Copy Address") && cb.get_text(&got) && got.view().equals("/") &&
            ex.status_label().text().equals("Copied the address."));
      CHECK(choose_context(w, ex.address_menu(), bar.x + 6, bar.y + 12, "Edit Address") && ex.address_bar().editing());
      press(w, key::kEscape);
      // From the keyboard, on the selected item.
      ex.view().select_item(0);
      w.set_focus(&ex.view());
      press(w, 0x65);
      CHECK(menu.open() && enabled(menu, "Open"));
      press(w, key::kEscape);
      CHECK(choose(w, ex.menu_bar(), "File", "Close"));
    }

    // --- Task manager: Copy Row.
    {
      desktop::ComputerManager tm(a);
      FakeMonitor mon;
      tm.set_monitor(&mon);
      CHECK(mgr.add_window(&tm));
      tm.update(1000000);
      w.layout();
      const Point o = tm.grid().window_origin();
      const Rect2D body = tm.grid().body_rect();
      const int32_t rx = o.x + body.x + 30, ry = o.y + body.y + 5 + DataGrid::kRowHeight;   // the second row
      right_click(w, o.x + 20, o.y + 5);                           // the header: nothing selected, nothing to copy
      CHECK(tm.grid_menu().open() && tm.grid().selected_row() == -1 && !enabled(tm.grid_menu(), "Copy Row"));
      w.close_popup();
      CHECK(choose_context(w, tm.grid_menu(), rx, ry, "Copy Row") && tm.grid().selected_row() >= 0 && cb.get_text(&got));
      CHECK(got.view().find('\t') != StringView::npos && got.view().find("idle") != StringView::npos);
      CHECK(choose(w, tm.menu_bar(), "File", "Close"));
    }

    // --- Paint: the right button paints, except with the Select tool, where it opens the menu.
    {
      desktop::Paint paint(a);
      CHECK(mgr.add_window(&paint));
      w.layout();
      DrawingCanvas& c = paint.canvas();
      ContextMenu& menu = paint.canvas_menu();
      paint.palette().set_secondary(0xFF00FF00);
      right_click(w, pixel_point(c, 20, 20).x, pixel_point(c, 20, 20).y);
      CHECK(!menu.open() && !w.popup());
      drag(w, pixel_point(c, 20, 20), pixel_point(c, 30, 20), MouseButton::Right);
      CHECK(c.pixel(25, 20) == 0xFF00FF00 && !menu.open());      // painted with the secondary color
      paint.tools().select_tool(7);
      const Point at = pixel_point(c, 100, 100);
      right_click(w, at.x, at.y);
      CHECK(menu.open() && enabled(menu, "Undo") && !enabled(menu, "Redo") && !enabled(menu, "Cut") && !enabled(menu, "Delete") && enabled(menu, "Copy") && !enabled(menu, "Paste"));
      w.close_popup();
      c.select_rect(10, 10, 30, 30);
      CHECK(choose_context(w, menu, at.x, at.y, "Copy") && cb.has_image());
      right_click(w, at.x, at.y);
      CHECK(enabled(menu, "Cut") && enabled(menu, "Delete") && enabled(menu, "Paste"));
      w.close_popup();
      CHECK(choose_context(w, menu, at.x, at.y, "Delete") && c.pixel(25, 20) == 0xFF00FF00 && c.has_selection());
      CHECK(choose_context(w, menu, at.x, at.y, "Undo") && c.pixel(25, 20) == 0xFF00FF00);
      CHECK(choose_context(w, menu, at.x, at.y, "Select All") && c.has_selection() && c.selection().width == 640);
      CHECK(choose_context(w, menu, at.x, at.y, "Paste") && c.floating());
      CHECK(choose(w, paint.menu_bar(), "File", "Close"));
    }
    w.render();
  }
  CHECK(a.live == 0);
}

// Where a program's window is: the tabbed window around it (a program is a tab of one), or the frame itself.
static void put(Frame* f, const Rect2D& r) { (f->host() ? f->host() : f)->set_bounds(r); }
static void put(Frame& f, const Rect2D& r) { put(&f, r); }

// Several windows of one program, and files dragged from one file explorer to another and onto an editor.
static Point item_point(allocators::IAllocator& a, desktop::FileExplorer& e, const char* name) {
  for (uint32_t i = 0; i < e.view().item_count(); ++i) {
    String text(a);
    e.view().item_text(i, 0, &text);
    Rect2D row;
    if (text.view().equals(name) && e.view().item_rect(i, &row)) {
      const Point o = e.view().window_origin();
      return {o.x + row.x + 30, o.y + row.y + 5};
    }
  }
  return {-1, -1};
}

static void test_desktop_multiple_windows() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    make_file(fs, "/a.txt", "dragged");
    CHECK(fs.create_directory("/dest") == filesystem::Status::Ok);
    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.set_file_system(&fs);
    Window& w = d.window();

    // Every Start menu entry opens one more window; closing the extra ones destroys them.
    desktop::FileExplorer* ex1 = d.open_explorer();
    CHECK(ex1 == &d.explorer() && d.explorer_count() == 1);
    put(ex1, {0, 0, 380, 250});
    desktop::FileExplorer* ex2 = d.open_explorer();
    CHECK(ex2 && ex2 != ex1 && d.explorer_count() == 2 && d.windows().window_count() == 2);
    put(ex2, {400, 0, 380, 250});
    CHECK(ex2->open_folder("/dest"));
    put(d.open_text_editor(), {0, 270, 300, 300});
    desktop::Notepad* second = d.open_text_editor();
    CHECK(second != &d.notepad() && d.text_editor_count() == 2);
    put(second, {310, 270, 300, 300});
    w.layout();

    // Drag a.txt from the first explorer over to the second one: it moves into /dest, and both lists show it.
    const Point from = item_point(a, *ex1, "a.txt");
    CHECK(from.x >= 0);
    w.mouse_move(from.x, from.y);
    w.mouse_button(from.x, from.y, MouseButton::Left, true);
    w.mouse_move(from.x + 3, from.y);
    CHECK(!w.dragging());                                  // too short for a drag yet
    w.mouse_move(500, 150);
    CHECK(w.dragging() && w.drag_text().equals("/a.txt"));
    w.mouse_button(500, 150, MouseButton::Left, false);
    CHECK(!w.dragging());
    filesystem::DirectoryEntry entry;
    CHECK(fs.stat("/dest/a.txt", &entry) == filesystem::Status::Ok && fs.stat("/a.txt", &entry) == filesystem::Status::NotFound);
    CHECK(item_point(a, *ex1, "a.txt").x < 0 && item_point(a, *ex2, "a.txt").x >= 0);

    // Escape cancels a drag; dropping on a place that takes nothing does nothing.
    const Point back = item_point(a, *ex2, "a.txt");
    w.mouse_move(back.x, back.y);
    w.mouse_button(back.x, back.y, MouseButton::Left, true);
    w.mouse_move(back.x, back.y + 40);
    CHECK(w.dragging());
    KeyArgs esc;
    esc.usage = hardware::key::kEscape;
    w.key(esc);
    CHECK(!w.dragging());
    w.mouse_button(back.x, back.y + 40, MouseButton::Left, false);
    CHECK(fs.stat("/dest/a.txt", &entry) == filesystem::Status::Ok);

    // Dropped on the second text editor, the file opens there and nowhere else.
    w.mouse_move(back.x, back.y);
    w.mouse_button(back.x, back.y, MouseButton::Left, true);
    w.mouse_move(450, 400);
    CHECK(w.dragging());
    w.mouse_button(450, 400, MouseButton::Left, false);
    CHECK(second->path().equals("/dest/a.txt") && second->editor().text().equals("dragged") && d.notepad().path().empty());

    // A folder cannot be dropped into itself, and a name that is taken is not overwritten.
    CHECK(!ex1->move_into("/dest", "/dest"));
    make_file(fs, "/a.txt", "other");
    CHECK(!ex1->move_into("/a.txt", "/dest") && fs.stat("/a.txt", &entry) == filesystem::Status::Ok);

    // Opening a file from an explorer uses an editor that is empty, or opens a new one when all hold something.
    d.step(1000000);
    CHECK(d.text_editor_count() == 2);
    ex2->close();
    second->close();
    d.step(2000000);
    CHECK(d.explorer_count() == 1 && d.text_editor_count() == 1);
    CHECK(d.notice_dialog().showing());       // (the refused moves above said why) - besides it only the first explorer and editor remain
    CHECK(d.windows().window_count() == 3);
    CHECK(d.open_text_editor() != &d.notepad());
    CHECK(d.text_editor_count() == 2);
  }
  CHECK(a.live == 0);
}

// A filesystem that cannot rename, like two volumes: moving between folders has to copy, which shows the progress window.
class NoRenameFs final : public filesystem::IFileSystem {
 public:
  explicit NoRenameFs(filesystem::IFileSystem& inner) : inner_(inner) {}
  filesystem::FileSystemInfo info() const override { return inner_.info(); }
  filesystem::Status open_file(StringView p, filesystem::OpenMode m, UniquePointer<filesystem::IFile>* f) override { return inner_.open_file(p, m, f); }
  filesystem::Status open_directory(StringView p, UniquePointer<filesystem::IDirectory>* d) override { return inner_.open_directory(p, d); }
  filesystem::Status stat(StringView p, filesystem::DirectoryEntry* e) override { return inner_.stat(p, e); }
  filesystem::Status create_directory(StringView p) override { return inner_.create_directory(p); }
  filesystem::Status remove(StringView p) override { return inner_.remove(p); }
  filesystem::Status rename(StringView, StringView) override { return filesystem::Status::Unsupported; }
  filesystem::Status flush() override { return inner_.flush(); }

 private:
  filesystem::IFileSystem& inner_;
};

struct ProgressLog {
  int files = 0, chunks = 0;
  uint64_t last_done = 0, last_total = 0;
  static void on_file(void* c, StringView, uint64_t, uint64_t) { ++static_cast<ProgressLog*>(c)->files; }
  static void on_bytes(void* c, uint64_t done, uint64_t total) {
    auto* l = static_cast<ProgressLog*>(c);
    ++l->chunks;
    l->last_done = done;
    l->last_total = total;
  }
};

static void test_copy_progress() {
  TestAllocator a;
  {
    filesystem::RamFileSystem ram(a);
    NoRenameFs fs(ram);
    CHECK(ram.create_directory("/top") == filesystem::Status::Ok && ram.create_directory("/top/src") == filesystem::Status::Ok);
    CHECK(ram.create_directory("/dest") == filesystem::Status::Ok);
    make_file(ram, "/top/src/a.txt", "hello");
    make_file(ram, "/top/src/b.txt", "world!!");
    filesystem::DirectoryEntry entry;

    // The transfer engine: measures, reports each file and the bytes, and stops when told to.
    {
      desktop::FileTransfer t(a, fs);
      ProgressLog log;
      t.set_callbacks(&ProgressLog::on_file, &ProgressLog::on_bytes, &log);
      CHECK(t.measure("/top/src") == filesystem::Status::Ok && t.files_total() == 2 && t.bytes_total() == 12);
      CHECK(t.copy("/top/src", "/copy") == filesystem::Status::Ok && log.files == 2 && log.chunks == 2 && log.last_done == 12 && log.last_total == 12);
      CHECK(fs.stat("/copy/b.txt", &entry) == filesystem::Status::Ok && entry.size == 7);
      bool stop = true;
      t.set_cancel_flag(&stop);
      CHECK(t.copy("/top/src", "/copy2") == desktop::FileTransfer::kCancelled);
      CHECK(desktop::FileTransfer::remove_tree(a, fs, "/copy") == filesystem::Status::Ok && fs.stat("/copy", &entry) == filesystem::Status::NotFound);
    }

    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.set_file_system(&fs);
    Window& w = d.window();
    desktop::FileExplorer* ex1 = d.open_explorer();
    put(ex1, {0, 0, 380, 250});
    desktop::FileExplorer* ex2 = d.open_explorer();
    put(ex2, {400, 0, 380, 250});
    CHECK(ex1->open_folder("/top") && ex2->open_folder("/dest"));
    w.layout();

    // Dragging a folder to the other explorer copies it with the progress window, then removes the original; both explorers are refreshed.
    const Point from = item_point(a, *ex1, "src");
    CHECK(from.x >= 0);
    w.mouse_move(from.x, from.y);
    w.mouse_button(from.x, from.y, MouseButton::Left, true);
    w.mouse_move(500, 150);
    w.mouse_button(500, 150, MouseButton::Left, false);
    desktop::CopyProgressDialog& progress = d.copy_progress();
    CHECK(progress.wait_until_idle(1000000) && !progress.busy() && progress.manager() == nullptr);
    CHECK(progress.result() == filesystem::Status::Ok && progress.message().empty());
    CHECK(progress.bar().value() == 12 && progress.bar().maximum() == 12 && progress.bar().percent() == 100);
    CHECK(progress.heading_label().text().first(7).equals("Removin") || progress.heading_label().text().first(6).equals("Moving"));
    CHECK(fs.stat("/dest/src/b.txt", &entry) == filesystem::Status::Ok && fs.stat("/top/src", &entry) == filesystem::Status::NotFound);
    CHECK(item_point(a, *ex1, "src").x < 0 && item_point(a, *ex2, "src").x >= 0);

    // A copy that fails reports it in a notice and leaves no half copy behind.
    CHECK(ram.create_directory("/top/src") == filesystem::Status::Ok);
    make_file(ram, "/top/src/c.txt", "x");
    CHECK(!ex1->move_into("/top/src", "/dest") && !d.copy_progress().busy());   // the name is taken
    CHECK(d.notice_dialog().showing());
  }
  CHECK(a.live == 0);
}

// The terminal: the shell's commands, typing on the command line, and dropping a folder on the window.
static bool terminal_shows(desktop::Terminal& t, const char* text) {
  for (uint32_t i = 0; i < t.view().line_count(); ++i)
    if (t.view().line(i).find(StringView(text)) != StringView::npos) return true;
  return false;
}

static void test_terminal() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    CHECK(fs.create_directory("/docs") == filesystem::Status::Ok && fs.create_directory("/docs/deep") == filesystem::Status::Ok);
    make_file(fs, "/docs/a.txt", "alpha");
    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.set_file_system(&fs);
    Window& w = d.window();
    desktop::Terminal* t = d.open_terminal();
    CHECK(t == &d.terminal() && d.terminal_count() == 1);
    desktop::Terminal* other = d.open_terminal();
    CHECK(other != t && d.terminal_count() == 2);
    put(other, {0, 320, 300, 200});
    put(t, {400, 0, 380, 300});
    w.layout();

    // Commands, typed with the keyboard on the line; the prompt shows the folder.
    CHECK(t->view().prompt().equals("/> "));
    auto type = [&](const char* text) {
      w.set_focus(&t->view());
      for (const char* c = text; *c; ++c) {
        KeyArgs k;
        k.character = *c;
        w.key(k);
      }
      KeyArgs enter;
      enter.usage = hardware::key::kEnter;
      w.key(enter);
    };
    type("cd docs");
    CHECK(t->shell().cwd().equals("/docs") && t->view().prompt().equals("/docs> ") && t->title().equals("Terminal - /docs"));
    type("ls -l");
    CHECK(terminal_shows(*t, "deep/") && terminal_shows(*t, "a.txt"));
    type("cat a.txt");
    CHECK(terminal_shows(*t, "alpha"));
    type("mkdir \"two words\"");
    type("cp a.txt \"two words\"");
    filesystem::DirectoryEntry entry;
    CHECK(fs.stat("/docs/two words/a.txt", &entry) == filesystem::Status::Ok);
    type("mv a.txt deep/b.txt");
    CHECK(fs.stat("/docs/deep/b.txt", &entry) == filesystem::Status::Ok && fs.stat("/docs/a.txt", &entry) == filesystem::Status::NotFound);
    type("cd ../docs/deep/..");
    CHECK(t->shell().cwd().equals("/docs"));
    type("cd nowhere");
    CHECK(terminal_shows(*t, "cd: nowhere: not found") && t->shell().cwd().equals("/docs"));
    type("rm deep");
    CHECK(terminal_shows(*t, "rm: deep: that is a folder") && fs.stat("/docs/deep", &entry) == filesystem::Status::Ok);
    type("rm -r deep");
    CHECK(fs.stat("/docs/deep", &entry) == filesystem::Status::NotFound);
    type("nonsense");
    CHECK(terminal_shows(*t, "nonsense: command not found"));
    // History: Up brings the last command back.
    KeyArgs up;
    up.usage = hardware::key::kUp;
    w.key(up);
    CHECK(t->view().input().equals("nonsense"));
    t->view().set_input(StringView());
    type("clear");
    CHECK(t->view().line_count() == 1);

    // A folder dragged from a file explorer to the terminal becomes the current folder; a file puts its path on the line.
    t->shell().change_directory("/");
    desktop::FileExplorer* ex = d.open_explorer();
    put(ex, {0, 0, 380, 300});
    CHECK(ex->open_folder("/docs"));
    make_file(fs, "/docs/note.txt", "n");
    ex->refresh();
    w.layout();
    const Point folder = item_point(a, *ex, "two words");
    CHECK(folder.x >= 0);
    w.mouse_move(folder.x, folder.y);
    w.mouse_button(folder.x, folder.y, MouseButton::Left, true);
    w.mouse_move(550, 150);
    w.mouse_button(550, 150, MouseButton::Left, false);
    CHECK(t->shell().cwd().equals("/docs/two words") && terminal_shows(*t, "cd \"/docs/two words\""));
    const Point file = item_point(a, *ex, "note.txt");
    w.mouse_move(file.x, file.y);
    w.mouse_button(file.x, file.y, MouseButton::Left, true);
    w.mouse_move(550, 150);
    w.mouse_button(550, 150, MouseButton::Left, false);
    CHECK(t->view().input().equals("/docs/note.txt "));

    // Save Output As writes the screen text to a file; exit closes the window.
    CHECK(t->save_output("/out.log") && fs.stat("/out.log", &entry) == filesystem::Status::Ok && entry.size > 10);
    t->view().set_input(StringView());
    type("exit");
    d.step(1000000);
    CHECK(d.terminal_count() == 1);
  }
  CHECK(a.live == 0);
}

// The dock buttons in the title bar snap a window to a half of the desktop area, and the same button again puts it back.
static void test_dock_buttons() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    Frame f(a), dialog(a);
    f.set_bounds({100, 80, 300, 200});
    dialog.set_bounds({10, 10, 200, 100});
    dialog.set_maximize_enabled(false);
    CHECK(mgr.add_window(&f) && mgr.add_window(&dialog));
    w.layout();
    CHECK(f.dock_enabled() && !dialog.dock_enabled());
    auto press = [&](Frame::Part part) {
      const Rect2D r = f.button_rect(part);
      const Point o = f.window_origin();
      click(w, o.x + r.x + r.width / 2, o.y + r.y + r.height / 2);
    };
    // Left to right: the four dock buttons, then Minimize, Maximize, Close.
    CHECK(f.button_rect(Frame::Part::DockLeft).x < f.button_rect(Frame::Part::DockRight).x && f.button_rect(Frame::Part::DockBottom).x < f.button_rect(Frame::Part::Minimize).x);
    press(Frame::Part::DockLeft);
    CHECK(f.docked() == Frame::Dock::Left && f.bounds().x == 0 && f.bounds().y == 0 && f.bounds().width == 400 && f.bounds().height == 600);
    press(Frame::Part::DockRight);
    CHECK(f.docked() == Frame::Dock::Right && f.bounds().x == 400 && f.bounds().width == 400 && f.bounds().height == 600);
    press(Frame::Part::DockTop);
    CHECK(f.docked() == Frame::Dock::Top && f.bounds().x == 0 && f.bounds().width == 800 && f.bounds().height == 300);
    press(Frame::Part::DockBottom);
    CHECK(f.docked() == Frame::Dock::Bottom && f.bounds().y == 300 && f.bounds().width == 800 && f.bounds().height == 300);
    press(Frame::Part::DockBottom);   // again: back to where it was before the first docking
    CHECK(f.docked() == Frame::Dock::None && f.bounds().x == 100 && f.bounds().y == 80 && f.bounds().width == 300 && f.bounds().height == 200);
    // Dragging by the title bar frees a docked window; maximize then restore returns to the undocked size.
    f.dock(Frame::Dock::Left);
    const Point o = f.window_origin();
    w.mouse_move(o.x + 20, o.y + 10);
    w.mouse_button(o.x + 20, o.y + 10, MouseButton::Left, true);
    w.mouse_move(o.x + 120, o.y + 60);
    w.mouse_button(o.x + 120, o.y + 60, MouseButton::Left, false);
    CHECK(f.docked() == Frame::Dock::None && f.bounds().x == 100);
    f.dock(Frame::Dock::Right);
    f.maximize();
    CHECK(f.state() == Frame::State::Maximized && f.docked() == Frame::Dock::None);
    f.maximize();
    CHECK(f.state() == Frame::State::Normal && f.bounds().x == 100 && f.bounds().width == 400);
    dialog.dock(Frame::Dock::Left);
    CHECK(dialog.docked() == Frame::Dock::None);
  }
  CHECK(a.live == 0);
}

// Alt+Tab goes through the open windows, Shift+Alt+Tab backwards, and the Super key opens and closes the Start menu.
static void test_desktop_shortcuts() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.open_text_editor();
    d.open_paint();
    d.open_explorer();
    d.window().layout();
    Window& w = d.window();
    gui::WindowManager& m = d.windows();
    Frame* e = &d.notepad();
    Frame* p = &d.paint();
    Frame* x = &d.explorer();
    CHECK(m.active() == x->host());
    auto press = [&](uint8_t usage, uint8_t modifiers) {
      KeyArgs k;
      k.usage = usage;
      k.modifiers = modifiers;
      w.key(k);
    };
    const uint8_t alt = hardware::modifier::kLeftAlt, shift = hardware::modifier::kLeftShift;
    press(hardware::key::kTab, alt);
    CHECK(m.active() == e->host() && m.stacking_index(e->host()) == 2);
    press(hardware::key::kTab, alt);
    CHECK(m.active() == p->host());
    press(hardware::key::kTab, alt);
    CHECK(m.active() == x->host());                                   // all the way round
    press(hardware::key::kTab, alt | shift);
    CHECK(m.active() == p->host());                                   // backwards: the window behind the front one
    press(hardware::key::kTab, alt | shift);
    CHECK(m.active() == e->host());
    // A minimized window is brought back by going forwards to it.
    e->minimize();
    CHECK(e->state() == Frame::State::Minimized);
    for (int i = 0; i < 3 && e->state() == Frame::State::Minimized; ++i) press(hardware::key::kTab, alt);
    CHECK(e->state() == Frame::State::Normal && m.active() == e->host());
    // Tab without Alt is still an ordinary key.
    Frame* before = m.active();
    press(hardware::key::kTab, 0);
    CHECK(m.active() == before);

    CHECK(!d.start_button().open());
    press(hardware::key::kLeftGui, hardware::modifier::kLeftGui);
    CHECK(d.start_button().open() && w.popup() != nullptr);
    press(hardware::key::kRightGui, hardware::modifier::kRightGui);
    CHECK(!d.start_button().open() && w.popup() == nullptr);
    // A modal dialog keeps both to itself.
    d.ask_shut_down();
    press(hardware::key::kLeftGui, hardware::modifier::kLeftGui);
    CHECK(!d.start_button().open());
  }
  CHECK(a.live == 0);
}

// Clicking a path in the terminal's output opens a folder in a file explorer and a file in the program for it.
static void test_terminal_links() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    CHECK(fs.create_directory("/docs") == filesystem::Status::Ok && fs.create_directory("/docs/deep") == filesystem::Status::Ok);
    make_file(fs, "/docs/a.txt", "alpha");
    make_file(fs, "/docs/pic.bmp", "x");
    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.set_file_system(&fs);
    Window& w = d.window();
    desktop::Terminal* t = d.open_terminal();
    put(t, {300, 0, 480, 500});
    t->view().clear();
    w.layout();
    t->run("cd docs");
    t->run("ls");
    t->run("echo see /docs/deep, or \"/docs/a.txt\" plain");
    w.layout();
    // The click point of the first occurrence of `text` in the output (nothing is scrolled: it all fits).
    auto point_of = [&](const char* text) {
      for (uint32_t i = 0; i < t->view().line_count(); ++i) {
        const uint64_t at = t->view().line(i).find(StringView(text));
        if (at == StringView::npos) continue;
        const Point o = t->view().window_origin();
        return Point{o.x + gui::TerminalView::kMargin + static_cast<int32_t>(at) * 8 + 4, o.y + gui::TerminalView::kMargin + static_cast<int32_t>(i) * 16 + 6};
      }
      return Point{-1, -1};
    };
    // Words that are no path do nothing, and nothing is underlined for them.
    Point p = point_of("plain");
    CHECK(p.x >= 0);
    click(w, p.x, p.y);
    CHECK(d.explorer_count() == 0 && d.text_editor_count() == 0);
    // A folder in the output of ls (relative to the current folder) opens in a file explorer at that folder.
    p = point_of("deep/");
    CHECK(p.x >= 0);
    w.mouse_move(p.x, p.y);
    uint64_t line, from, to;
    String word(a);
    CHECK(t->view().link_at(p.x - t->view().window_origin().x, p.y - t->view().window_origin().y, &line, &from, &to, &word) && word.view().equals("deep/"));
    click(w, p.x, p.y);
    CHECK(d.explorer_count() == 1 && d.explorer().path().equals("/docs/deep"));
    // An absolute path with punctuation behind it, and a quoted one. (The explorer is moved aside: it opened over the terminal.)
    put(d.explorer(), {0, 520, 200, 80});
    w.layout();
    p = point_of("/docs/deep,");
    click(w, p.x, p.y);
    CHECK(d.explorer_count() == 1 && d.explorer().path().equals("/docs/deep"));
    // A picture opens in Paint (a relative name counts from the folder the terminal is in now).
    p = point_of("pic.bmp");
    CHECK(p.x >= 0);
    click(w, p.x, p.y);
    CHECK(d.paint_count() == 1);
    if (d.notice_dialog().showing()) d.notice_dialog().ok_button().click();   // (the file is not a real picture)
    put(d.paint(), {0, 520, 200, 80});
    t->run("cd ..");
    w.layout();
    p = point_of("/docs/a.txt");
    CHECK(p.x >= 0);
    click(w, p.x + 8, p.y);
    CHECK(d.text_editor_count() == 1 && d.notepad().path().equals("/docs/a.txt") && d.notepad().editor().text().equals("alpha"));
    // Dragging across text is not a click.
    d.notepad().close();
    p = point_of("/docs/a.txt");
    w.mouse_move(p.x, p.y);
    w.mouse_button(p.x, p.y, MouseButton::Left, true);
    w.mouse_move(p.x + 40, p.y);
    w.mouse_button(p.x + 40, p.y, MouseButton::Left, false);
    CHECK(d.text_editor_count() == 0);
  }
  CHECK(a.live == 0);
}

// Every program is a tab of a tabbed window: "+" opens another tab, tabs are dragged between windows and out of them, closed, and reordered.
static Point tab_point(Frame* host, uint32_t index, int32_t dx = 20) {
  auto* h = static_cast<gui::TabFrame*>(host);
  const Rect2D r = h->tab_rect(index);
  const Point o = h->window_origin();
  return {o.x + r.x + dx, o.y + r.y + static_cast<int32_t>(r.height) / 2};
}

static void test_tabs() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    Screen screen(a, 1000, 700);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.set_file_system(&fs);
    Window& w = d.window();
    gui::WindowManager& m = d.windows();

    // A program opens in a window with one tab; the window's title is the tab's.
    desktop::Notepad* n1 = d.open_text_editor();
    auto* h1 = static_cast<gui::TabFrame*>(n1->host());
    CHECK(h1 && n1->embedded() && h1->tab_count() == 1 && h1->selected() == n1 && m.window_count() == 1);
    CHECK(h1->title().equals(n1->title()) && n1->manager() == &m && n1->state() == Frame::State::Normal && n1->active());
    put(h1, {20, 20, 480, 300});
    w.layout();

    // The tab fills the area under the strip.
    CHECK(n1->bounds().y >= gui::TabFrame::kStripHeight && n1->visible());

    // Editing changes the title: the tab strip and the window follow.
    d.notepad().editor().set_text("x");
    d.notepad().open_file("/nothing");   // fails with a notice, harmless
    if (d.notice_dialog().showing()) d.notice_dialog().ok_button().click();
    make_file(fs, "/f.txt", "file");
    CHECK(d.notepad().open_file("/f.txt") && h1->title().equals(d.notepad().title()));

    // "+": a second text editor tab in the same window; the new one is selected, the old one hidden.
    const Rect2D plus = h1->new_tab_button_rect();
    const Point po = h1->window_origin();
    click(w, po.x + plus.x + 8, po.y + plus.y + 6);
    CHECK(h1->tab_count() == 2 && m.window_count() == 1 && d.text_editor_count() == 2);
    desktop::Notepad* n2 = static_cast<desktop::Notepad*>(h1->tab_at(1));
    CHECK(n2 != n1 && h1->selected() == n2 && n2->visible() && !n1->visible() && n2->app_kind() == n1->app_kind());
    CHECK(h1->title().equals(n2->title()));
    // Clicking the first tab shows it again.
    Point tp = tab_point(h1, 0);
    click(w, tp.x, tp.y);
    CHECK(h1->selected() == n1 && n1->visible() && !n2->visible());
    // Ctrl+T and Ctrl+Tab.
    KeyArgs ct;
    ct.usage = hardware::key::kA + 19;
    ct.modifiers = hardware::modifier::kLeftControl;
    w.set_focus(&n1->editor());
    w.key(ct);
    CHECK(h1->tab_count() == 3);
    KeyArgs next;
    next.usage = hardware::key::kTab;
    next.modifiers = hardware::modifier::kLeftControl;
    w.key(next);
    CHECK(h1->selected() == h1->tab_at(0));

    // Dragging the third tab out of the window onto the desktop makes a window of its own.
    Frame* third = h1->tab_at(2);
    tp = tab_point(h1, 2);
    w.mouse_move(tp.x, tp.y);
    w.mouse_button(tp.x, tp.y, MouseButton::Left, true);
    w.mouse_move(tp.x + 100, tp.y + 300);
    CHECK(w.dragging());
    w.mouse_button(tp.x + 100, tp.y + 300, MouseButton::Left, false);
    CHECK(m.window_count() == 2 && h1->tab_count() == 2 && third->host() && third->host() != h1);
    auto* h2 = static_cast<gui::TabFrame*>(third->host());
    CHECK(h2->tab_count() == 1 && h2->selected() == third && h2->manager() == &m && third->manager() == &m);
    put(h2, {520, 20, 440, 300});
    w.layout();

    // A window with a single tab cannot be torn off: nothing happens (the window itself is dragged by its title bar).
    tp = tab_point(h2, 0);
    w.mouse_move(tp.x, tp.y);
    w.mouse_button(tp.x, tp.y, MouseButton::Left, true);
    w.mouse_move(tp.x + 20, tp.y + 200);
    w.mouse_button(tp.x + 20, tp.y + 200, MouseButton::Left, false);
    CHECK(m.window_count() == 2 && h2->tab_count() == 1);

    // Dropping a tab on another window's strip moves it there, at the place it was let go.
    Frame* moved = h1->tab_at(1);
    tp = tab_point(h1, 1);
    w.mouse_move(tp.x, tp.y);
    w.mouse_button(tp.x, tp.y, MouseButton::Left, true);
    Point strip = tab_point(h2, 0, 4);   // the left half of the only tab: before it
    w.mouse_move(strip.x + 30, strip.y);
    w.mouse_move(strip.x, strip.y);
    CHECK(w.dragging());
    w.mouse_button(strip.x, strip.y, MouseButton::Left, false);
    CHECK(h1->tab_count() == 1 && h2->tab_count() == 2 && h2->tab_at(0) == moved && h2->tab_at(1) == third && h2->selected() == moved);
    CHECK(moved->host() == h2 && moved->manager() == &m && h2->title().equals(moved->title()));

    // Reordering inside a strip.
    tp = tab_point(h2, 0);
    w.mouse_move(tp.x, tp.y);
    w.mouse_button(tp.x, tp.y, MouseButton::Left, true);
    const Point end = tab_point(h2, 1, 120);
    w.mouse_move(end.x, end.y);
    w.mouse_button(end.x, end.y, MouseButton::Left, false);
    CHECK(h2->tab_at(0) == third && h2->tab_at(1) == moved);

    // Moving the last tab of a window into another closes the emptied window.
    Frame* only = h1->tab_at(0);
    tp = tab_point(h1, 0);
    w.mouse_move(tp.x, tp.y);
    w.mouse_button(tp.x, tp.y, MouseButton::Left, true);
    strip = tab_point(h2, 1, 100);
    w.mouse_move(strip.x + 40, strip.y);
    w.mouse_move(strip.x, strip.y);
    w.mouse_button(strip.x, strip.y, MouseButton::Left, false);
    CHECK(h2->tab_count() == 3 && only->host() == h2 && m.window_count() == 1 && h1->manager() == nullptr);

    // Escape cancels a drag.
    tp = tab_point(h2, 1);
    w.mouse_move(tp.x, tp.y);
    w.mouse_button(tp.x, tp.y, MouseButton::Left, true);
    w.mouse_move(tp.x, tp.y + 250);
    CHECK(w.dragging());
    KeyArgs esc;
    esc.usage = hardware::key::kEscape;
    w.key(esc);
    w.mouse_button(tp.x, tp.y + 250, MouseButton::Left, false);
    CHECK(h2->tab_count() == 3 && m.window_count() == 1);

    // Closing: the close button of a tab, then the last tab closes the window; extra editors are cleaned up.
    d.step(1000000);
    while (h2->tab_count() > 1) {
      const Rect2D c = h2->close_rect(h2->tab_count() - 1);
      const Point o = h2->window_origin();
      click(w, o.x + c.x + 5, o.y + c.y + 5);
    }
    CHECK(h2->tab_count() == 1 && m.window_count() == 1);
    Frame* last = h2->tab_at(0);
    const Rect2D c = h2->close_rect(0);
    const Point o = h2->window_origin();
    click(w, o.x + c.x + 5, o.y + c.y + 5);
    CHECK(m.window_count() == 0 && last->host() == nullptr && last->manager() == nullptr);
    d.step(2000000);
    CHECK(d.text_editor_count() == 0);
    // Everything can be opened again afterwards.
    CHECK(d.open_text_editor() && d.text_editor_count() == 1 && m.window_count() == 1);

    // Closing a window closes its tabs; a program closing itself (File > Close) closes only its tab.
    gui::TabFrame* h3 = static_cast<gui::TabFrame*>(d.open_terminal()->host());
    d.open_terminal(h3);
    CHECK(h3->tab_count() == 2 && d.terminal_count() == 2);
    h3->tab_at(1)->close();
    CHECK(h3->tab_count() == 1 && d.terminal_count() == 1);
    d.open_terminal(h3);
    h3->close();
    CHECK(h3->manager() == nullptr && d.terminal_count() == 0 && h3->tab_count() == 0);
    d.step(3000000);
    CHECK(d.terminal_count() == 0);
  }
  CHECK(a.live == 0);
}

// The tile button arranges the windows in a 2x2 grid; pressed again by a tiled window it puts them all back.
static void test_tile_button() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    Frame f[6] = {Frame(a), Frame(a), Frame(a), Frame(a), Frame(a), Frame(a)};
    Frame dialog(a);
    dialog.set_maximize_enabled(false);
    for (int i = 0; i < 6; ++i) {
      f[i].set_bounds({10 + 20 * i, 10 + 15 * i, 300, 200});
      CHECK(mgr.add_window(&f[i]));
    }
    CHECK(mgr.add_window(&dialog));
    dialog.close();   // (a window that cannot be resized never joins in, whether open or not)
    CHECK(mgr.add_window(&dialog));
    w.layout();
    CHECK(f[0].button_rect(Frame::Part::Tile).x < f[0].button_rect(Frame::Part::DockLeft).x && !dialog.dock_enabled());
    auto press_tile = [&](Frame& frame) {
      const Rect2D r = frame.button_rect(Frame::Part::Tile);
      const Point o = frame.window_origin();
      click(w, o.x + r.x + r.width / 2, o.y + r.y + r.height / 2);
    };
    // Six windows: the four on top (f[5] is the last opened, so the topmost; the dialog stays) are put in the quadrants, top to bottom of the stack
    // in reading order; the two below are not touched.
    mgr.activate(&f[5]);
    press_tile(f[5]);
    CHECK(f[5].docked() == Frame::Dock::Tile && f[5].bounds().x == 0 && f[5].bounds().y == 0 && f[5].bounds().width == 400 && f[5].bounds().height == 300);
    CHECK(f[4].docked() == Frame::Dock::Tile && f[4].bounds().x == 400 && f[4].bounds().y == 0 && f[4].bounds().width == 400 && f[4].bounds().height == 300);
    CHECK(f[3].docked() == Frame::Dock::Tile && f[3].bounds().x == 0 && f[3].bounds().y == 300 && f[3].bounds().width == 400 && f[3].bounds().height == 300);
    CHECK(f[2].docked() == Frame::Dock::Tile && f[2].bounds().x == 400 && f[2].bounds().y == 300 && f[2].bounds().width == 400 && f[2].bounds().height == 300);
    CHECK(f[1].docked() == Frame::Dock::None && f[0].docked() == Frame::Dock::None && f[1].bounds().x == 30 && dialog.docked() == Frame::Dock::None);
    CHECK(mgr.active() == &f[5]);
    // Pressed again in a tiled window: all of them go back.
    press_tile(f[5]);
    CHECK(f[5].docked() == Frame::Dock::None && f[5].bounds().x == 110 && f[5].bounds().y == 85 && f[5].bounds().width == 300);
    CHECK(f[2].docked() == Frame::Dock::None && f[2].bounds().x == 50 && f[3].bounds().y == 55);

    // Fewer windows: one fills the area, two a half each, three have two quarters over a half. Minimized windows do not count.
    f[0].minimize();
    f[1].minimize();
    f[2].minimize();
    f[3].minimize();
    f[4].minimize();
    mgr.tile(&f[5]);
    CHECK(f[5].bounds().x == 0 && f[5].bounds().y == 0 && f[5].bounds().width == 800 && f[5].bounds().height == 600);
    mgr.restore(&f[5]);
    f[4].restore();
    mgr.tile(&f[4]);
    CHECK(f[4].bounds().x == 0 && f[4].bounds().width == 400 && f[4].bounds().height == 600 && f[5].bounds().x == 400 && f[5].bounds().width == 400);
    mgr.tile(&f[4]);
    f[3].restore();
    f[4].activate();
    mgr.tile(&f[3]);
    Frame* top_three[3] = {&f[3], &f[4], &f[5]};
    int quarters = 0, halves = 0;
    for (Frame* t : top_three) {
      quarters += t->bounds().width == 400 && t->bounds().height == 300;
      halves += t->bounds().width == 800 && t->bounds().height == 300;
    }
    CHECK(quarters == 2 && halves == 1);
    mgr.tile(&f[3]);

    // A maximized window is tiled too and goes back to its size from before the maximizing; a docked one likewise.
    f[5].maximize();
    f[4].dock(Frame::Dock::Left);
    mgr.tile(&f[5]);
    CHECK(f[5].docked() == Frame::Dock::Tile && f[5].state() == Frame::State::Normal);
    mgr.tile(&f[5]);
    CHECK(f[5].docked() == Frame::Dock::None && f[5].bounds().width == 300 && f[4].docked() == Frame::Dock::None && f[4].bounds().width == 300);

    // The grid follows the size of the area.
    mgr.tile(&f[5]);
    mgr.set_bounds({0, 0, 600, 400});
    mgr.layout();
    CHECK(f[5].docked() == Frame::Dock::Tile);
    uint32_t sum = 0;
    for (Frame* t : top_three) sum += t->bounds().width;
    CHECK(sum >= 900 && f[5].bounds().width <= 600);
    // And it toggles back.
    mgr.tile(&f[5]);
    CHECK(f[5].docked() == Frame::Dock::None);
  }
  CHECK(a.live == 0);
}

// The mouse wheel over the tab strip goes through the tabs; over the taskbar's window buttons through the windows.
static void test_wheel_switching() {
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    Screen screen(a, 1000, 700);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.set_file_system(&fs);
    Window& w = d.window();
    gui::WindowManager& m = d.windows();
    desktop::Notepad* n1 = d.open_text_editor();
    auto* h1 = static_cast<gui::TabFrame*>(n1->host());
    d.open_text_editor(h1);
    d.open_text_editor(h1);
    put(h1, {20, 20, 480, 300});
    w.layout();
    CHECK(h1->tab_count() == 3 && h1->selected() == h1->tab_at(2));
    const Point o = h1->window_origin();
    const Rect2D strip = h1->strip_rect();
    const int32_t sx = o.x + strip.x + 30, sy = o.y + strip.y + 10;
    w.mouse_move(sx, sy);
    w.mouse_wheel(sx, sy, 1);   // up: the previous tab
    CHECK(h1->selected() == h1->tab_at(1));
    w.mouse_wheel(sx, sy, 1);
    CHECK(h1->selected() == h1->tab_at(0));
    w.mouse_wheel(sx, sy, 1);   // (stops at the first)
    CHECK(h1->selected() == h1->tab_at(0));
    w.mouse_wheel(sx, sy, -1);
    w.mouse_wheel(sx, sy, -1);
    w.mouse_wheel(sx, sy, -1);
    CHECK(h1->selected() == h1->tab_at(2));
    // Over the page of a tab the wheel is the page's.
    const Point body = {o.x + 100, o.y + strip.y + strip.height + 30};
    w.mouse_wheel(body.x, body.y, 1);
    CHECK(h1->selected() == h1->tab_at(2));

    // Windows: three more, then the wheel over the taskbar.
    d.open_paint();
    d.open_explorer();
    w.layout();
    gui::WindowSwitcher& sw = d.switcher();
    CHECK(sw.button_count() == 3);   // (the text editors share a window)
    const Point so = sw.window_origin();
    const int32_t wx = so.x + 20, wy = so.y + 8;
    Frame* first = sw.frame_at(0);
    Frame* second = sw.frame_at(1);
    Frame* third = sw.frame_at(2);
    CHECK(m.active() == third);
    w.mouse_move(wx, wy);
    w.mouse_wheel(wx, wy, 1);
    CHECK(m.active() == second);
    w.mouse_wheel(wx, wy, 1);
    CHECK(m.active() == first);
    w.mouse_wheel(wx, wy, 1);
    CHECK(m.active() == first);          // the first one is the end
    w.mouse_wheel(wx, wy, -1);
    CHECK(m.active() == second && m.stacking_index(second) == 2);
    w.mouse_wheel(wx, wy, -1);
    w.mouse_wheel(wx, wy, -1);
    CHECK(m.active() == third);
    // A minimized window is brought back by scrolling to it; with the window in front minimized the wheel starts from the end it came from.
    first->minimize();
    third->minimize();
    second->minimize();
    CHECK(m.active() != first && m.active() != second && m.active() != third);
    w.mouse_wheel(wx, wy, -1);
    CHECK(m.active() == first && first->state() == Frame::State::Normal);
    // Not while a dialog wants an answer.
    d.ask_shut_down();
    const Frame* before = m.active();
    CHECK(m.has_modal());
    w.mouse_wheel(wx, wy, -1);
    w.mouse_wheel(wx, wy, 1);
    CHECK(m.active() == before && m.has_modal());
  }
  CHECK(a.live == 0);
}

static void test_window_switcher_menu() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    d.open_text_editor();
    d.open_paint();
    d.open_explorer();
    d.window().layout();
    WindowSwitcher& sw = d.switcher();
    Window& w = d.window();
    CHECK(sw.button_count() == 3);
    auto button_point = [&](uint32_t i) {
      const Point o = sw.window_origin();
      const Rect2D r = sw.button_rect(i);
      return Point{o.x + r.x + static_cast<int32_t>(r.width) / 2, o.y + r.y + static_cast<int32_t>(r.height) / 2};
    };
    // Right-clicking a button opens the menu of that window.
    Point p = button_point(0);
    right_click(w, p.x, p.y);
    CHECK(sw.window_menu().open() && sw.menu_frame() == d.notepad().host());
    CHECK(!sw.window_menu().find_item("Restore")->enabled() && sw.window_menu().find_item("Minimize")->enabled() && sw.window_menu().find_item("Maximize")->enabled() &&
          sw.window_menu().find_item("Close")->enabled());
    w.close_popup();
    CHECK(d.notepad().state() == Frame::State::Normal);
    CHECK(choose_context(w, sw.window_menu(), p.x, p.y, "Minimize") && d.notepad().state() == Frame::State::Minimized);
    right_click(w, p.x, p.y);
    CHECK(sw.window_menu().find_item("Restore")->enabled() && !sw.window_menu().find_item("Minimize")->enabled());
    w.close_popup();
    CHECK(choose_context(w, sw.window_menu(), p.x, p.y, "Restore") && d.notepad().state() == Frame::State::Normal && d.windows().active() == d.notepad().host());
    p = button_point(1);
    CHECK(choose_context(w, sw.window_menu(), p.x, p.y, "Maximize") && d.paint().state() == Frame::State::Maximized && sw.menu_frame() == d.paint().host());
    right_click(w, p.x, p.y);
    CHECK(sw.window_menu().find_item("Restore")->enabled() && !sw.window_menu().find_item("Maximize")->enabled());
    w.close_popup();
    CHECK(choose_context(w, sw.window_menu(), p.x, p.y, "Restore") && d.paint().state() == Frame::State::Normal);
    p = button_point(2);
    CHECK(choose_context(w, sw.window_menu(), p.x, p.y, "Close") && sw.button_count() == 2 && d.explorer().manager() == nullptr);
    // An empty part of the switcher has no menu, and neither does it while a dialog is up.
    right_click(w, sw.window_origin().x + static_cast<int32_t>(sw.bounds().width) - 3, sw.window_origin().y + 5);
    CHECK(!sw.window_menu().open());
    d.ask_shut_down();
    p = button_point(0);
    right_click(w, p.x, p.y);
    CHECK(!sw.window_menu().open());
    d.shutdown_dialog().close();
    // From the keyboard it is the active window's.
    w.set_focus(&sw);
    press(w, 0x65);
    CHECK(sw.window_menu().open() && sw.menu_frame() == d.windows().active());
    press(w, key::kEscape);
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
  test_file_dialog();
  test_desktop_file_dialog();
  test_desktop_multiple_windows();
  test_copy_progress();
  test_terminal();
  test_terminal_links();
  test_dock_buttons();
  test_tile_button();
  test_wheel_switching();
  test_desktop_shortcuts();
  test_tabs();
  test_spin_box();
  test_color_spectrum();
  test_color_dialog();
  test_print_preview();
  test_print_dialog();
  test_print_and_color_flows();
  test_app_menus();
  test_context_menu_control();
  test_text_context_menus();
  test_application_context_menus();
  test_window_switcher_menu();
  test_select_all_keys();
  test_breadcrumb_bar();
  test_file_explorer();
  test_explorer_network();
  test_explorer_nfs();
  test_explorer_sftp_ftps();
  test_computer_manager();
  test_tree_view();
  test_hardware_tab();
  test_network_properties();
  test_display_settings_entry();
  test_text_editor();
  test_notepad();
  test_media_player();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gui views: all checks passed\n");
  return 0;
}
