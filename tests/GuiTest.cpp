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

// Host-side tests for the GUI toolkit: docking layout, the even-split container, input routing,
// and the button, text box, drop-down and menu controls, driven by synthetic input and rendered
// with the software 2D renderer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Desktop/Desktop.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/MenuButton.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/InputPump.hpp"
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
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
static bool is(const Rect2D& r, int32_t x, int32_t y, uint32_t w, uint32_t h) { return r.x == x && r.y == y && r.width == w && r.height == h; }

// A screen to render to.
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

// Typing helpers.
static void press(Window& w, uint8_t usage, uint8_t modifiers = 0) {
  KeyArgs k;
  k.usage = usage; k.modifiers = modifiers; k.character = hardware::key_to_ascii(usage, modifiers);
  w.key(k);
}
static void type(Window& w, const char* text) {
  for (; *text; ++text) {
    KeyArgs k;
    k.character = *text;
    k.usage = *text >= 'a' && *text <= 'z' ? static_cast<uint8_t>(key::kA + (*text - 'a')) : 0;
    w.key(k);
  }
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

static void test_dock_layout() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 400, 300);
    Window w(a, screen.renderer, theme, {400, 300});
    Panel top(a), bottom(a), left(a), right(a), fill(a);
    top.set_dock(DockStyle::Top); top.set_size(0, 30);
    bottom.set_dock(DockStyle::Bottom); bottom.set_size(0, 20);
    left.set_dock(DockStyle::Left); left.set_size(100, 0);
    right.set_dock(DockStyle::Right); right.set_size(80, 0);
    fill.set_dock(DockStyle::Fill);
    CHECK(w.add(&top) && w.add(&bottom) && w.add(&left) && w.add(&right) && w.add(&fill));
    CHECK(!w.add(&top));  // already in a tree
    w.layout();
    CHECK(is(top.bounds(), 0, 0, 400, 30));
    CHECK(is(bottom.bounds(), 0, 280, 400, 20));
    CHECK(is(left.bounds(), 0, 30, 100, 250));
    CHECK(is(right.bounds(), 320, 30, 80, 250));
    CHECK(is(fill.bounds(), 100, 30, 220, 250));

    // Nested: a panel inside the fill area docks its own children, with padding.
    Panel inner(a); Button ok(a);
    inner.set_dock(DockStyle::Fill);
    inner.set_padding(Insets::all(5));
    ok.set_dock(DockStyle::Bottom); ok.set_size(0, 24);
    CHECK(fill.add(&inner) && inner.add(&ok));
    w.layout();
    CHECK(is(inner.bounds(), 0, 0, 220, 250));
    CHECK(is(ok.bounds(), 5, 221, 210, 24));
    CHECK(ok.window_origin().x == 105 && ok.window_origin().y == 251);

    // A strip bigger than the space left is cut down; what is after it gets nothing.
    top.set_size(0, 1000);
    w.layout();
    CHECK(is(top.bounds(), 0, 0, 400, 300));
    CHECK(left.bounds().height == 0 && fill.bounds().height == 0);
    top.set_size(0, 30);

    // Hidden controls take no space; None keeps its own bounds.
    left.set_visible(false);
    Panel floating(a);
    floating.set_bounds({7, 8, 9, 10});
    w.add(&floating);
    w.layout();
    CHECK(is(fill.bounds(), 0, 30, 320, 250));
    CHECK(is(floating.bounds(), 7, 8, 9, 10));
    CHECK(w.remove(&floating) && !w.remove(&floating));

    // A title bar moves the client area down.
    w.set_title("Test");
    w.layout();
    CHECK(is(top.bounds(), 0, 20, 400, 30));
    CHECK(is(bottom.bounds(), 0, 280, 400, 20));
    CHECK(w.hit_test(10, 5) == &w);
    CHECK(w.hit_test(10, 25) == &top);
    CHECK(w.hit_test(500, 5) == nullptr);
  }
  CHECK(a.live == 0);
}

static void test_uniform_panel() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 200, 100);
    Window w(a, screen.renderer, theme, {200, 100});
    UniformPanel row(a);
    row.set_dock(DockStyle::Fill);
    Button b1(a), b2(a), b3(a);
    row.add(&b1); row.add(&b2); row.add(&b3);
    w.add(&row);
    w.layout();
    CHECK(is(b1.bounds(), 0, 0, 67, 100) && is(b2.bounds(), 67, 0, 67, 100) && is(b3.bounds(), 134, 0, 66, 100));
    b2.set_visible(false);
    w.layout();
    CHECK(is(b1.bounds(), 0, 0, 100, 100) && is(b3.bounds(), 100, 0, 100, 100));
    b2.set_visible(true);
    row.set_orientation(Orientation::Vertical);
    row.set_spacing(4);
    w.layout();
    // 100 - 2*4 = 92 over three: 31, 31, 30.
    CHECK(is(b1.bounds(), 0, 0, 200, 31) && is(b2.bounds(), 0, 35, 200, 31) && is(b3.bounds(), 0, 70, 200, 30));
    row.set_padding(Insets::all(10));
    row.set_spacing(0);
    w.layout();
    CHECK(is(b1.bounds(), 10, 10, 180, 27) && is(b3.bounds(), 10, 64, 180, 26));
    // Evenly split containers nest with docking.
    UniformPanel columns(a);
    Panel left(a), right(a);
    columns.add(&left); columns.add(&right);
    row.remove(&b3);
    row.add(&columns);
    w.layout();
    CHECK(left.bounds().width + right.bounds().width == columns.bounds().width);
    CHECK(left.bounds().width - right.bounds().width <= 1);
  }
  CHECK(a.live == 0);
}

static void test_button_and_focus() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 200);
    Window w(a, screen.renderer, theme, {300, 200});
    Button ok(a), cancel(a);
    TextBox name(a);
    ok.set_text("OK"); cancel.set_text("Cancel");
    ok.set_dock(DockStyle::Top); cancel.set_dock(DockStyle::Top); name.set_dock(DockStyle::Top);
    w.add(&ok); w.add(&cancel); w.add(&name);
    w.layout();
    CHECK(ok.bounds().height == 24 && cancel.bounds().y == 24 && name.bounds().y == 48);
    Counter clicks;
    ok.on_click(clicks.handler());
    click_center(w, ok);
    CHECK(clicks.count == 1 && clicks.last == &ok);
    CHECK(w.focus() == &ok && ok.focused());
    // Press, drag off, release elsewhere: no click. Drag off and back: click.
    Point p = center(ok);
    w.mouse_move(p.x, p.y);
    w.mouse_button(p.x, p.y, MouseButton::Left, true);
    w.mouse_move(p.x, p.y + 100);
    w.mouse_button(p.x, p.y + 100, MouseButton::Left, false);
    CHECK(clicks.count == 1);
    w.mouse_button(p.x, p.y, MouseButton::Left, true);
    w.mouse_move(p.x, p.y + 100);
    w.mouse_move(p.x, p.y);
    w.mouse_button(p.x, p.y, MouseButton::Left, false);
    CHECK(clicks.count == 2);
    // Right button does nothing; disabled buttons ignore everything.
    w.mouse_button(p.x, p.y, MouseButton::Right, true);
    w.mouse_button(p.x, p.y, MouseButton::Right, false);
    CHECK(clicks.count == 2);
    ok.set_enabled(false);
    click_center(w, ok);
    press(w, key::kSpace);
    CHECK(clicks.count == 2);
    ok.set_enabled(true);
    // Keyboard: Space activates the focused button; Tab walks the focusable controls and wraps.
    press(w, key::kSpace);
    CHECK(clicks.count == 3);
    press(w, key::kTab);
    CHECK(w.focus() == &cancel);
    press(w, key::kTab);
    CHECK(w.focus() == &name);
    press(w, key::kTab);
    CHECK(w.focus() == &ok);
    press(w, key::kTab, hardware::modifier::kLeftShift);
    CHECK(w.focus() == &name);
    // Removing the focused control clears the focus.
    w.remove(&name);
    CHECK(w.focus() == nullptr);
  }
  CHECK(a.live == 0);
}

static void test_textbox() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 200, 100);
    Window w(a, screen.renderer, theme, {200, 100});
    TextBox box(a);
    box.set_dock(DockStyle::Top);
    w.add(&box);
    w.layout();
    Counter changes;
    box.on_changed(changes.handler());
    click_center(w, box);
    CHECK(w.focus() == &box);
    type(w, "helo");
    CHECK(box.text().equals("helo") && box.caret() == 4 && changes.count == 4);
    press(w, key::kLeft);
    type(w, "l");
    CHECK(box.text().equals("hello") && box.caret() == 4);
    press(w, 0x4A);  // Home
    CHECK(box.caret() == 0);
    press(w, key::kBackspace);  // nothing before the caret
    CHECK(box.text().equals("hello"));
    press(w, 0x4C);  // Delete
    CHECK(box.text().equals("ello"));
    press(w, 0x4D);  // End
    press(w, key::kBackspace);
    CHECK(box.text().equals("ell") && box.caret() == 3);
    press(w, key::kRight);
    CHECK(box.caret() == 3);
    // Shift gives capitals and symbols; Ctrl+letter types nothing.
    press(w, key::kA, hardware::modifier::kLeftShift);
    CHECK(box.text().equals("ellA"));
    press(w, key::kA, hardware::modifier::kLeftControl);
    CHECK(box.text().equals("ellA"));
    // Clicking places the caret between characters.
    const Point o = box.window_origin();
    click(w, o.x + 4 + 2 * 8 + 1, o.y + 10);
    CHECK(box.caret() == 2);
    click(w, o.x + 2, o.y + 10);
    CHECK(box.caret() == 0);
    // Limits, read-only, Enter/Tab fall through to the window.
    box.set_text("");
    box.set_max_length(3);
    type(w, "abcd");
    CHECK(box.text().equals("abc"));
    box.set_read_only(true);
    press(w, key::kBackspace);
    type(w, "x");
    CHECK(box.text().equals("abc"));
    KeyArgs enter; enter.usage = key::kEnter;
    CHECK(!box.on_key(enter));
    // Long text scrolls so the caret stays visible, and renders without touching outside the control.
    box.set_read_only(false);
    box.set_max_length(0);
    box.set_text("The quick brown fox jumps over the lazy dog");
    CHECK(box.caret() == 43);
    w.render();
    press(w, 0x4A);
    w.render();
    CHECK(box.caret() == 0);
  }
  CHECK(a.live == 0);
}

static void test_dropdown() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 300, 300);
    Window w(a, screen.renderer, theme, {300, 300});
    DropDown dd(a);
    Panel below(a);
    dd.set_dock(DockStyle::Top);
    below.set_dock(DockStyle::Fill);
    CHECK(dd.add_item("Red") == 0 && dd.add_item("Green") == 1 && dd.add_item("Blue") == 2);
    w.add(&dd); w.add(&below);
    w.layout();
    Counter changes;
    dd.on_selection_changed(changes.handler());
    CHECK(dd.selected_index() == -1 && dd.selected_text().size() == 0);
    // Click opens the list below the control, above the panel that is there.
    click_center(w, dd);
    CHECK(dd.open() && w.popup() != nullptr);
    const Rect2D list = w.popup()->bounds();
    CHECK(list.x == 0 && list.y == static_cast<int32_t>(dd.bounds().height) && list.width == dd.bounds().width && list.height == 3 * 18 + 4);
    // Hover hit-testing and click select the item under the pointer.
    click(w, 10, list.y + 2 + 18 + 5);
    CHECK(!dd.open() && w.popup() == nullptr);
    CHECK(dd.selected_index() == 1 && dd.selected_text().equals("Green") && changes.count == 1);
    // Clicking outside closes without a change and the click does not fall through to what is under it.
    Counter under;
    click_center(w, dd);
    CHECK(dd.open());
    click(w, 150, 200);
    CHECK(!dd.open() && dd.selected_index() == 1 && changes.count == 1);
    // A second click on the drop-down itself closes it too.
    click_center(w, dd);
    click_center(w, dd);
    CHECK(!dd.open() && w.popup() == nullptr);
    // Keyboard on the closed control: arrows change the selection (clamped), Alt+Down opens.
    press(w, key::kDown);
    CHECK(dd.selected_index() == 2 && changes.count == 2);
    press(w, key::kDown);
    CHECK(dd.selected_index() == 2 && changes.count == 2);
    press(w, key::kUp);
    CHECK(dd.selected_index() == 1);
    press(w, key::kDown, hardware::modifier::kLeftAlt);
    CHECK(dd.open());
    press(w, key::kDown);   // popup gets the keys
    press(w, key::kDown);   // wraps to the first
    press(w, key::kEnter);
    CHECK(!dd.open() && dd.selected_index() == 0);
    press(w, key::kSpace);
    CHECK(dd.open());
    press(w, key::kEscape);
    CHECK(!dd.open() && dd.selected_index() == 0);
    // Removing the owner while open closes the popup.
    click_center(w, dd);
    CHECK(w.popup());
    w.remove(&dd);
    CHECK(w.popup() == nullptr);
    // The popup is drawn above the controls (here: the panel below the drop-down).
    w.remove(&below);
    w.add(&dd);
    w.add(&below);  // Fill controls go last
    w.layout();
    click_center(w, dd);
    w.render();
    const Rect2D l2 = w.popup()->bounds();
    CHECK(screen.px(l2.x + 60, l2.y + 40) == (theme.field & 0xFFFFFFFF));
    dd.clear_items();
    CHECK(dd.item_count() == 0 && dd.selected_index() == -1 && w.popup() == nullptr);
  }
  CHECK(a.live == 0);
}

static void test_menu() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 400, 300);
    Window w(a, screen.renderer, theme, {400, 300});
    MenuBar bar(a);
    Panel body(a);
    bar.set_dock(DockStyle::Top);
    body.set_dock(DockStyle::Fill);
    Menu* file = bar.add_menu("File");
    Menu* edit = bar.add_menu("Edit");
    Counter open_item, exit_item, undo_item, disabled_item;
    file->add_item("Open", open_item.handler(), "Ctrl+O");
    MenuItem* off = file->add_item("Save", disabled_item.handler());
    off->enabled = false;
    file->add_separator();
    file->add_item("Exit", exit_item.handler());
    MenuItem* undo = edit->add_item("Undo", undo_item.handler(), "Ctrl+Z");
    undo->checked = true;
    w.add(&bar); w.add(&body);
    w.layout();
    CHECK(bar.bounds().height == 22 && bar.bounds().width == 400 && bar.menu_count() == 2);

    // Click the File title: its menu opens below the bar.
    click(w, 12, 10);
    CHECK(bar.open_menu_index() == 0 && w.popup() != nullptr);
    CHECK(w.popup()->bounds().y == 21);
    const Rect2D p = w.popup()->bounds();
    // Rows: Open 20, Save 20, separator 8, Exit 20 (+2 top margin).
    const int32_t open_y = p.y + 2 + 10, save_y = p.y + 2 + 20 + 10, sep_y = p.y + 2 + 40 + 4, exit_y = p.y + 2 + 48 + 10;
    // Disabled items and separators do nothing, and keep the menu open.
    click(w, p.x + 20, save_y);
    click(w, p.x + 20, sep_y);
    CHECK(disabled_item.count == 0 && w.popup() != nullptr);
    // Sliding along the bar switches menus.
    w.mouse_move(50, 10);
    CHECK(bar.open_menu_index() == 1);
    w.mouse_move(12, 10);
    CHECK(bar.open_menu_index() == 0);
    // Choosing an item runs it and closes the menu.
    click(w, p.x + 20, exit_y);
    CHECK(exit_item.count == 1 && w.popup() == nullptr && bar.open_menu_index() == -1);
    CHECK(open_item.count == 0);
    // Click outside closes without running anything and the click does not reach the body.
    click(w, 12, 10);
    CHECK(w.popup());
    click(w, 200, 200);
    CHECK(w.popup() == nullptr && exit_item.count == 1);
    // Clicking the open title closes the menu.
    click(w, 12, 10);
    click(w, 12, 10);
    CHECK(w.popup() == nullptr);
    // Keyboard: open, move, run. Up/Down skip the disabled item and the separator.
    click(w, 12, 10);
    press(w, key::kDown);   // Open
    press(w, key::kDown);   // skips Save and the separator: Exit
    press(w, key::kDown);   // wraps to Open
    press(w, key::kEnter);
    CHECK(open_item.count == 1 && w.popup() == nullptr);
    click(w, 12, 10);
    press(w, key::kUp);     // nothing highlighted yet: the last item (Exit)
    press(w, key::kSpace);
    CHECK(exit_item.count == 2);
    // Left/Right switch menus; Escape closes.
    click(w, 12, 10);
    press(w, key::kRight);
    CHECK(bar.open_menu_index() == 1 && w.popup() != nullptr);
    press(w, key::kDown);
    press(w, key::kEnter);
    CHECK(undo_item.count == 1 && undo_item.last == &bar);
    click(w, 50, 10);
    press(w, key::kLeft);
    CHECK(bar.open_menu_index() == 0);
    press(w, key::kEscape);
    CHECK(w.popup() == nullptr && bar.open_menu_index() == -1);
    // Rendering: the popup covers the body, and the highlighted title uses the selection color.
    click(w, 12, 10);
    w.render();
    CHECK(screen.px(12, 2) == theme.selection);
    CHECK(screen.px(p.x + p.width - 4, p.y + 12) == theme.face);
    // A menu near the right edge is moved to stay on screen.
    Menu* late = bar.add_menu("Right-most menu with a long title");
    late->add_item("A fairly long item", {}, "Ctrl+X");
    w.layout();
    CHECK(bar.open_menu_index() == 0);  // File is still open from the rendering check
    w.mouse_move(250, 10);
    CHECK(bar.open_menu_index() == 2);
    CHECK(w.popup()->bounds().x + static_cast<int32_t>(w.popup()->bounds().width) <= 400);
    // Removing the bar closes its menu.
    w.remove(&bar);
    CHECK(w.popup() == nullptr);
  }
  CHECK(a.live == 0);
}

static void test_rendering() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 320, 240);
    Window w(a, screen.renderer, theme, {320, 240});
    w.set_title("Demo");
    MenuBar bar(a);
    Button b(a);
    b.set_text("Hello");
    bar.set_dock(DockStyle::Top);
    b.set_dock(DockStyle::Top);
    bar.add_menu("File");
    w.add(&bar); w.add(&b);
    w.render();
    CHECK(!w.dirty());
    CHECK(screen.px(3, 3) == theme.title);              // title bar
    CHECK(screen.px(300, 100) == theme.face);           // window background
    const Rect2D br = b.bounds();
    CHECK(screen.px(br.x, br.y) == theme.highlight);    // raised bevel: lit top-left edge
    CHECK(screen.px(br.x + br.width - 1, br.y + br.height - 1) == theme.dark);
    // Pressing sinks the button.
    const Point c = center(b);
    w.mouse_move(c.x, c.y);
    w.mouse_button(c.x, c.y, MouseButton::Left, true);
    CHECK(w.dirty());
    w.render();
    CHECK(screen.px(br.x, br.y) == theme.shadow);
    w.mouse_button(c.x, c.y, MouseButton::Left, false);
    // Cursor overlay.
    w.show_cursor(true);
    w.mouse_move(100, 150);
    w.render();
    CHECK(screen.px(102, 156) == theme.dark);
  }
  CHECK(a.live == 0);
}

struct FakeKeyboard final : hardware::IKeyboard {
  hardware::KeyEvent events[16]; int head = 0, tail = 0; uint8_t mods = 0;
  const char* name() const override { return "fake keyboard"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool poll(hardware::KeyEvent* out) override { if (head == tail) return false; *out = events[head++]; return true; }
  uint8_t modifiers() const override { return mods; }
  void push(uint8_t usage, bool pressed) { events[tail++] = {usage, pressed}; }
};

struct FakeMouse final : hardware::IMouse {
  hardware::MouseEvent events[16]; int head = 0, tail = 0; bool absolute = false;
  const char* name() const override { return "fake mouse"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool poll(hardware::MouseEvent* out) override { if (head == tail) return false; *out = events[head++]; return true; }
  bool is_absolute() const override { return absolute; }
  uint32_t max_x() const override { return absolute ? 32767 : 0; }
  uint32_t max_y() const override { return absolute ? 32767 : 0; }
  void move(int32_t dx, int32_t dy, uint8_t buttons) { events[tail++] = {dx, dy, 0, buttons, false, 0, 0, 0, 0}; }
  void at(uint32_t x, uint32_t y, uint8_t buttons) { events[tail++] = {0, 0, 0, buttons, true, x, y, 32767, 32767}; }
};

static void test_input_pump() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 200, 100);
    Window w(a, screen.renderer, theme, {200, 100});
    Button b(a);
    TextBox t(a);
    b.set_dock(DockStyle::Top); t.set_dock(DockStyle::Top);
    w.add(&b); w.add(&t);
    w.layout();
    Counter clicks;
    b.on_click(clicks.handler());
    FakeKeyboard kb;
    FakeMouse mouse;
    InputPump pump(w, &kb, &mouse);
    CHECK(pump.cursor().x == 100 && pump.cursor().y == 50);
    // Relative: move to the button (top), press and release.
    mouse.move(-90, -45, 0);
    mouse.move(0, 0, hardware::mouse_button::kLeft);
    mouse.move(0, 0, 0);
    CHECK(pump.pump() == 3);
    CHECK(pump.cursor().x == 10 && pump.cursor().y == 5);
    CHECK(clicks.count == 1);
    // Movement is clamped to the window.
    mouse.move(-500, 500, 0);
    pump.pump();
    CHECK(pump.cursor().x == 0 && pump.cursor().y == 99);
    // Absolute (tablet): scaled to the window; clicking the text box focuses it, then keys type.
    mouse.absolute = true;
    mouse.at(16384, 6000, 0);   // a position only
    mouse.at(16384, 12000, hardware::mouse_button::kLeft);
    mouse.at(16384, 12000, 0);
    pump.pump();
    CHECK(w.focus() == &t);
    kb.push(key::kA + 7, true);   // h
    kb.push(key::kA + 7, false);
    CHECK(pump.pump() == 2);
    kb.mods = hardware::modifier::kLeftShift;
    kb.push(key::kA + 8, true);   // I
    CHECK(pump.pump() == 1);
    CHECK(t.text().equals("hI"));
  }
  CHECK(a.live == 0);
}

static void test_desktop() {
  TestAllocator a;
  {
    Screen screen(a, 640, 480);
    FakeKeyboard kb;
    FakeMouse mouse;
    desktop::Desktop d(a, screen.renderer, &kb, &mouse);
    gui::Window& w = d.window();
    gui::MenuButton& start = d.start_button();
    CHECK(!d.shutdown_requested());
    CHECK(d.step(0));  // first frame draws
    CHECK(!d.step(500000));  // nothing changed
    CHECK(d.step(1000000));  // the clock ticked
    // The taskbar runs along the bottom and the Start button sits at its left end.
    const Point o = start.window_origin();
    CHECK(o.x == 4 && o.y == 480 - desktop::Desktop::kTaskbarHeight + 4);
    CHECK(screen.px(300, 200) == gui::Theme().desktop);                 // desktop surface
    // Click Start with the mouse: a menu opens upwards, above the taskbar.
    mouse.absolute = true;
    mouse.at(1000, 31000, 0);   // (about) the Start button
    mouse.at(1000, 31000, hardware::mouse_button::kLeft);
    mouse.at(1000, 31000, 0);
    d.step(2000000);
    CHECK(start.open() && w.popup() != nullptr);
    const Rect2D menu = w.popup()->bounds();
    CHECK(menu.y + static_cast<int32_t>(menu.height) <= o.y && menu.x == o.x && menu.width >= 180);
    // Its only item shuts down: clicking elsewhere first just closes the menu.
    mouse.at(20000, 10000, hardware::mouse_button::kLeft);
    mouse.at(20000, 10000, 0);
    d.step(3000000);
    CHECK(!start.open() && !d.shutdown_requested());
    // Keyboard: Tab to the Start button, Enter opens, Enter chooses Shut Down.
    kb.push(key::kTab, true); kb.push(key::kTab, false);
    d.step(4000000);
    CHECK(w.focus() == &start);
    kb.push(key::kEnter, true); kb.push(key::kEnter, false);
    d.step(5000000);
    CHECK(start.open());
    kb.push(key::kDown, true); kb.push(key::kDown, false);
    kb.push(key::kEnter, true); kb.push(key::kEnter, false);
    d.step(6000000);
    CHECK(d.shutdown_requested() && !start.open());
  }
  CHECK(a.live == 0);
}

int main() {
  test_dock_layout();
  test_uniform_panel();
  test_button_and_focus();
  test_textbox();
  test_dropdown();
  test_menu();
  test_rendering();
  test_input_pump();
  test_desktop();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gui: all checks passed\n");
  return 0;
}
