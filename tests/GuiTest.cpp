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
#include <string>
#include <map>
#include <vector>
#include "Graphics/Graphics2D/ScaledRenderer2D.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Desktop/Desktop.hpp"
#include "Hardware/GraphicsProcessingUnit/Font8x16.hpp"
#include "Gui/Controls/ToastCenter.hpp"
#include "FakeMonitors.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/MenuButton.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Controls/TextEditor.hpp"
#include "Gui/InputPump.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Gui/Windows/WindowSwitcher.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/AllocatedView.hpp"
#include "X11/X11Server.hpp"

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
    off->set_enabled(false);
    file->add_separator();
    file->add_item("Exit", exit_item.handler());
    MenuItem* undo = edit->add_item("Undo", undo_item.handler(), "Ctrl+Z");
    undo->set_checked(true);
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
  hardware::KeyEvent events[128]; int head = 0, tail = 0; uint8_t mods = 0;
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

// A touchscreen whose range is the screen's pixels (max 799 x 599), so a contact lands on the pixel it names.
struct FakeTouch final : hardware::ITouch {
  std::vector<hardware::TouchEvent> events;
  bool emulation = true;
  const char* name() const override { return "fake touch"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool poll(hardware::TouchEvent* out) override {
    if (events.empty()) return false;
    *out = events.front();
    events.erase(events.begin());
    return true;
  }
  void set_pointer_emulation(bool on) override { emulation = on; }
  void put(hardware::TouchPhase phase, uint32_t id, uint32_t x, uint32_t y, uint32_t mx = 799, uint32_t my = 599) {
    hardware::TouchEvent e;
    e.phase = phase, e.id = id, e.x = x, e.y = y, e.max_x = mx, e.max_y = my;
    events.push_back(e);
  }
};

// A pen whose range is the screen's pixels.
struct FakePen final : hardware::IPen {
  std::vector<hardware::PenEvent> events;
  const char* name() const override { return "fake pen"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool poll(hardware::PenEvent* out) override {
    if (events.empty()) return false;
    *out = events.front();
    events.erase(events.begin());
    return true;
  }
  uint32_t max_x() const override { return 799; }
  uint32_t max_y() const override { return 599; }
  uint16_t max_pressure() const override { return 1000; }
  bool has_tilt() const override { return true; }
  void put(bool in_range, bool tip, uint32_t x, uint32_t y, uint8_t buttons = 0, uint16_t pressure = 0, uint32_t mx = 799, uint32_t my = 599) {
    hardware::PenEvent e;
    e.in_range = in_range, e.tip = tip, e.x = x, e.y = y, e.buttons = buttons, e.pressure = pressure, e.max_pressure = 1000, e.max_x = mx, e.max_y = my;
    events.push_back(e);
  }
};

// Pens in the input pump: offered to the tap first; one it does not take is the pointer (the tip the left button, the barrel the right one).
struct PenTapLog {
  int calls = 0;
  bool take = false;
  static bool tap(void* self, const hardware::PenEvent&, int32_t, int32_t) {
    auto* l = static_cast<PenTapLog*>(self);
    ++l->calls;
    return l->take;
  }
};

static void test_input_pump_pen() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 200, 100);
    Window w(a, screen.renderer, theme, {200, 100});
    Button b(a);
    b.set_bounds({10, 10, 60, 30});
    w.add(&b);
    w.layout();
    Counter clicks;
    b.on_click(clicks.handler());
    FakePen pen;
    PenTapLog log;
    InputPump pump(w, nullptr, nullptr);
    pump.set_pen(&pen, &PenTapLog::tap, &log);
    // hovering moves the pointer; the tip on the button and lifted clicks it
    pen.put(true, false, 150, 80, 0, 0, 199, 99);
    pump.pump();
    CHECK(pump.cursor().x == 150 && pump.cursor().y == 80 && clicks.count == 0);
    pen.put(true, false, 20, 20, 0, 0, 199, 99);
    pen.put(true, true, 20, 20, 0, 500, 199, 99);
    pen.put(true, false, 20, 20, 0, 0, 199, 99);
    pump.pump();
    CHECK(clicks.count == 1 && log.calls == 4);
    // the same report again: nothing for the pointer (no move, no click)
    pen.put(true, false, 20, 20, 0, 0, 199, 99);
    pump.pump();
    CHECK(clicks.count == 1);
    // taken by a program: the pointer does not move
    log.take = true;
    pen.put(true, true, 100, 50, 0, 500, 199, 99);
    pen.put(true, false, 100, 50, 0, 0, 199, 99);
    pump.pump();
    CHECK(pump.cursor().x == 20 && clicks.count == 1);
  }
  CHECK(a.live == 0);
}

// Touches in the input pump: the tap is asked first; a contact it does not take is the left button where it is (one at a time).
struct TouchTapLog {
  std::vector<std::string> calls;
  uint32_t take_id = 999;
  static bool tap(void* self, const hardware::TouchEvent& e, int32_t x, int32_t y) {
    auto* l = static_cast<TouchTapLog*>(self);
    l->calls.push_back(std::to_string(int(e.phase)) + " " + std::to_string(e.id) + " " + std::to_string(x) + " " + std::to_string(y));
    return e.id == l->take_id;
  }
};

static void test_input_pump_touch() {
  TestAllocator a;
  Theme theme;
  {
    Screen screen(a, 200, 100);
    Window w(a, screen.renderer, theme, {200, 100});
    Button b(a);
    b.set_bounds({10, 10, 60, 30});
    w.add(&b);
    w.layout();
    Counter clicks;
    b.on_click(clicks.handler());
    FakeTouch touch;
    TouchTapLog log;
    InputPump pump(w, nullptr, nullptr);
    pump.set_touch(&touch, &TouchTapLog::tap, &log);
    // a finger on the button, lifted: a click there; the tap was asked each time, in window coordinates
    touch.put(hardware::TouchPhase::Down, 1, 20, 20, 199, 99);
    touch.put(hardware::TouchPhase::Up, 1, 20, 20, 199, 99);
    CHECK(pump.pump() == 2);
    CHECK(clicks.count == 1 && pump.cursor().x == 20 && pump.cursor().y == 20);
    CHECK(log.calls.size() == 2 && log.calls[0] == "0 1 20 20" && log.calls[1] == "2 1 20 20");
    // a second finger while the first is down: not the button (only the first finger is)
    touch.put(hardware::TouchPhase::Down, 2, 150, 80, 199, 99);
    touch.put(hardware::TouchPhase::Down, 3, 20, 20, 199, 99);
    touch.put(hardware::TouchPhase::Up, 3, 20, 20, 199, 99);
    touch.put(hardware::TouchPhase::Up, 2, 150, 80, 199, 99);
    pump.pump();
    CHECK(clicks.count == 1 && pump.cursor().x == 150);
    // a finger the tap takes: no pointer at all
    log.take_id = 4;
    touch.put(hardware::TouchPhase::Down, 4, 20, 20, 199, 99);
    touch.put(hardware::TouchPhase::Up, 4, 20, 20, 199, 99);
    pump.pump();
    CHECK(clicks.count == 1 && pump.cursor().x == 150);
    // cancel lets the button go where the finger was (a mouse has no cancel: for the button it is a release, so a click)
    touch.put(hardware::TouchPhase::Down, 5, 20, 20, 199, 99);
    touch.put(hardware::TouchPhase::Cancel, 5, 20, 20, 199, 99);
    pump.pump();
    CHECK(clicks.count == 2);
  }
  CHECK(a.live == 0);
}

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
    kb.push(key::kLeftShift, true);
    kb.push(key::kA + 8, true);   // I
    kb.push(key::kA + 8, false);
    kb.push(key::kLeftShift, false);
    kb.mods = 0;                  // Shift is already up again when the pump reads the events: still a capital
    CHECK(pump.pump() == 4);
    CHECK(t.text().equals("hI"));
    kb.push(key::kA + 8, true);   // i
    CHECK(pump.pump() == 1);
    CHECK(t.text().equals("hIi"));
  }
  CHECK(a.live == 0);
}


// --- Window manager -------------------------------------------------------------

struct Closed {
  int count = 0;
  Frame* last = nullptr;
  static void fn(void* context, Control& sender) { auto* c = static_cast<Closed*>(context); ++c->count; c->last = static_cast<Frame*>(&sender); }
  Handler handler() { return {&Closed::fn, this}; }
};

static Point at_part(Frame& f, Frame::Part part) {  // a point (window coordinates) in a button or the title bar
  const Point o = f.window_origin();
  if (part == Frame::Part::TitleBar) return {o.x + 20, o.y + Frame::kBorder + 4};
  const Rect2D r = f.button_rect(part);
  return {o.x + r.x + static_cast<int32_t>(r.width) / 2, o.y + r.y + static_cast<int32_t>(r.height) / 2};
}

static void test_windows() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    Frame fa(a), fb(a), fc(a);
    fa.set_title("A"); fb.set_title("B"); fc.set_title("C");
    fa.set_bounds({10, 10, 300, 200});
    fb.set_bounds({100, 60, 300, 200});
    fc.set_bounds({200, 110, 300, 200});
    Closed closed;
    fc.on_closed(closed.handler());
    CHECK(mgr.add_window(&fa) && mgr.add_window(&fb) && mgr.add_window(&fc));
    CHECK(!mgr.add_window(&fa));
    w.layout();
    CHECK(mgr.window_count() == 3 && mgr.active() == &fc && fc.active() && !fa.active() && !fb.active());
    CHECK(mgr.stacking_index(&fa) == 0 && mgr.stacking_index(&fb) == 1 && mgr.stacking_index(&fc) == 2);

    // Stacking: where windows overlap the top one is hit; clicking a lower window raises and activates it.
    CHECK(w.hit_test(250, 150) == &fc);
    CHECK(w.hit_test(120, 70) == &fb);
    const Point on_a = at_part(fa, Frame::Part::TitleBar);
    w.mouse_move(on_a.x, on_a.y);
    w.mouse_button(on_a.x, on_a.y, MouseButton::Left, true);
    w.mouse_button(on_a.x, on_a.y, MouseButton::Left, false);
    CHECK(mgr.active() == &fa && mgr.stacking_index(&fa) == 2 && mgr.stacking_index(&fc) == 1);
    CHECK(fa.active() && !fc.active());
    // The creation order (for the taskbar) does not change.
    CHECK(mgr.window_at(0) == &fa && mgr.window_at(1) == &fb && mgr.window_at(2) == &fc);
    // Title bar colors: active vs inactive.
    w.render();
    CHECK(screen.px(fa.bounds().x + 8, fa.bounds().y + 8) == theme.title);
    CHECK(screen.px(fc.bounds().x + 120, fc.bounds().y + 8) == theme.inactive_title);

    // Minimize button: the window disappears, the next window below becomes active.
    const Point min_a = at_part(fa, Frame::Part::Minimize);
    click(w, min_a.x, min_a.y);
    CHECK(fa.state() == Frame::State::Minimized && !fa.visible() && mgr.active() == &fc);
    CHECK(w.hit_test(on_a.x, on_a.y) != &fa);
    mgr.activate(&fa);  // what the switcher does: restores it
    CHECK(fa.state() == Frame::State::Normal && fa.visible() && mgr.active() == &fa && mgr.stacking_index(&fa) == 2);

    // Maximize and restore.
    const Rect2D before = fa.bounds();
    const Point max_a = at_part(fa, Frame::Part::Maximize);
    click(w, max_a.x, max_a.y);
    CHECK(fa.state() == Frame::State::Maximized && fa.bounds().x == 0 && fa.bounds().y == 0 && fa.bounds().width == 800 && fa.bounds().height == 600);
    const Point restore_a = at_part(fa, Frame::Part::Maximize);
    click(w, restore_a.x, restore_a.y);
    CHECK(fa.state() == Frame::State::Normal && is(fa.bounds(), before.x, before.y, before.width, before.height));
    // A maximized window that was minimized comes back maximized.
    mgr.maximize(&fa);
    mgr.minimize(&fa);
    mgr.restore(&fa);
    CHECK(fa.state() == Frame::State::Maximized);
    mgr.maximize(&fa);

    // Disabled buttons ignore clicks; the others still work.
    fa.set_minimize_enabled(false);
    fa.set_maximize_enabled(false);
    fa.set_close_enabled(false);
    click(w, min_a.x, min_a.y);
    click(w, max_a.x, max_a.y);
    const Point close_a = at_part(fa, Frame::Part::Close);
    click(w, close_a.x, close_a.y);
    CHECK(fa.state() == Frame::State::Normal && fa.manager() == &mgr && mgr.window_count() == 3);
    fa.set_minimize_enabled(true);
    fa.set_maximize_enabled(true);
    fa.set_close_enabled(true);
    // Disabled-button glyphs are drawn in the shadow color.
    fa.set_maximize_enabled(false);
    w.render();
    const Rect2D mr = fa.button_rect(Frame::Part::Maximize);
    bool grey = false;
    for (uint32_t y = 0; y < mr.height; ++y)
      for (uint32_t x = 0; x < mr.width; ++x) grey = grey || screen.px(fa.bounds().x + mr.x + x, fa.bounds().y + mr.y + y) == theme.shadow;
    CHECK(grey);
    fa.set_maximize_enabled(true);

    // Dragging the title bar moves the window; it cannot be dragged out of reach.
    const Rect2D start = fa.bounds();
    const Point grab = at_part(fa, Frame::Part::TitleBar);
    w.mouse_move(grab.x, grab.y);
    w.mouse_button(grab.x, grab.y, MouseButton::Left, true);
    w.mouse_move(grab.x + 50, grab.y + 40);
    w.mouse_move(grab.x + 60, grab.y + 70);
    w.mouse_button(grab.x + 60, grab.y + 70, MouseButton::Left, false);
    CHECK(is(fa.bounds(), start.x + 60, start.y + 70, start.width, start.height));
    const Point grab2 = at_part(fa, Frame::Part::TitleBar);
    w.mouse_move(grab2.x, grab2.y);
    w.mouse_button(grab2.x, grab2.y, MouseButton::Left, true);
    w.mouse_move(grab2.x - 5000, grab2.y + 5000);
    w.mouse_button(grab2.x - 5000, grab2.y + 5000, MouseButton::Left, false);
    CHECK(fa.bounds().y == 600 - Frame::kBorder - Frame::kTitleHeight && fa.bounds().x + static_cast<int32_t>(fa.bounds().width) == 80);
    fa.set_bounds({10, 10, 300, 200});

    // Resizing: the bottom-right corner grows it, the top-left corner moves the origin; minimum size holds.
    const Point corner = {fa.window_origin().x + 299, fa.window_origin().y + 199};
    CHECK(fa.part_at(299, 199) == Frame::Part::BottomRight);
    w.mouse_move(corner.x, corner.y);
    w.mouse_button(corner.x, corner.y, MouseButton::Left, true);
    w.mouse_move(corner.x + 100, corner.y + 50);
    w.mouse_button(corner.x + 100, corner.y + 50, MouseButton::Left, false);
    CHECK(is(fa.bounds(), 10, 10, 400, 250));
    // The pointer shows the resize direction over the borders: the grab zone is kBorder wide.
    CHECK(fa.cursor_shape(100, 100) == CursorShape::Arrow && fa.cursor_shape(0, 100) == CursorShape::ResizeHorizontal &&
          fa.cursor_shape(Frame::kBorder - 1, 100) == CursorShape::ResizeHorizontal && fa.cursor_shape(Frame::kBorder, 100) != CursorShape::ResizeHorizontal);
    CHECK(fa.cursor_shape(200, 0) == CursorShape::ResizeVertical && fa.cursor_shape(1, 1) == CursorShape::ResizeDiagonalDown &&
          fa.cursor_shape(static_cast<int32_t>(fa.bounds().width) - 1, 1) == CursorShape::ResizeDiagonalUp);
    {
      w.show_cursor(true);  // drawn with the shape under it (ASAN checks the drawing stays on the screen)
      const Point edge = {fa.window_origin().x + static_cast<int32_t>(fa.bounds().width) - 2, fa.window_origin().y + 100};
      w.mouse_move(edge.x, edge.y);
      w.render();
      w.mouse_move(0, 0);
      w.render();
      w.mouse_move(799, 599);
      w.render();
      w.show_cursor(false);
    }
    // Dragging the right edge by a pixel inside the border widens the window.
    {
      const Point edge = {fa.window_origin().x + static_cast<int32_t>(fa.bounds().width) - 5, fa.window_origin().y + 100};
      const uint32_t before = fa.bounds().width;
      w.mouse_move(edge.x, edge.y);
      w.mouse_button(edge.x, edge.y, MouseButton::Left, true);
      w.mouse_move(edge.x + 30, edge.y);
      w.mouse_button(edge.x + 30, edge.y, MouseButton::Left, false);
      CHECK(fa.bounds().width == before + 30);
      fa.set_bounds({10, 10, 400, 250});
    }
    CHECK(fa.part_at(1, 1) == Frame::Part::TopLeft && fa.part_at(0, 100) == Frame::Part::Left && fa.part_at(200, 0) == Frame::Part::Top);
    const Point tl = {fa.window_origin().x + 1, fa.window_origin().y + 1};
    w.mouse_move(tl.x, tl.y);
    w.mouse_button(tl.x, tl.y, MouseButton::Left, true);
    w.mouse_move(tl.x + 5000, tl.y + 5000);
    w.mouse_button(tl.x + 5000, tl.y + 5000, MouseButton::Left, false);
    CHECK(fa.bounds().width == Frame::kMinWidth && fa.bounds().height == Frame::kMinHeight);
    CHECK(fa.bounds().x + static_cast<int32_t>(fa.bounds().width) == 410 && fa.bounds().y + static_cast<int32_t>(fa.bounds().height) == 260);
    fa.set_resizable(false);
    CHECK(fa.part_at(fa.bounds().width - 1, fa.bounds().height - 1) != Frame::Part::BottomRight);
    fa.set_resizable(true);
    fa.set_bounds({10, 10, 300, 200});

    // Close: removed from the manager, the handler runs, the next window below is active.
    mgr.activate(&fc);
    const Point close_c = at_part(fc, Frame::Part::Close);
    click(w, close_c.x, close_c.y);
    CHECK(closed.count == 1 && closed.last == &fc && fc.manager() == nullptr && mgr.window_count() == 2);
    CHECK(mgr.active() == &fb || mgr.active() == &fa);
    CHECK(mgr.add_window(&fc));  // a closed window can be opened again
    mgr.remove_window(&fc);

    // Children of a window are laid out inside its client area and receive input.
    fb.set_bounds({450, 300, 300, 200});  // clear of the other windows
    Button inner(a);
    Counter clicks;
    inner.set_dock(DockStyle::Top);
    inner.set_size(0, 30);
    inner.on_click(clicks.handler());
    fb.add(&inner);
    w.layout();
    CHECK(inner.bounds().x == Frame::kBorder && inner.bounds().y == Frame::kBorder + Frame::kTitleHeight + 1 && inner.bounds().width == fb.bounds().width - 2 * Frame::kBorder);
    click_center(w, inner);
    CHECK(clicks.count == 1 && mgr.active() == &fb && w.focus() == &inner);  // clicking inside activates the window
    // Tab stays inside the active window (and Window focus scope follows activation).
    TextBox other(a);
    other.set_dock(DockStyle::Top);
    fa.add(&other);
    w.layout();
    press(w, key::kTab);
    CHECK(w.focus() == &inner);
    mgr.activate(&fa);
    CHECK(w.focus() == &other);
    mgr.activate(&fb);
    CHECK(w.focus() == &inner);
    mgr.remove_window(&fa);
    mgr.remove_window(&fb);
    CHECK(mgr.window_count() == 0 && mgr.active() == nullptr);
  }
  CHECK(a.live == 0);
}

static void test_modal_and_dialog() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    Frame doc(a);
    doc.set_title("Document");
    doc.set_bounds({50, 50, 300, 200});
    Button save(a);
    save.set_dock(DockStyle::Top);
    save.set_size(0, 30);
    Counter saves;
    save.on_click(saves.handler());
    doc.add(&save);
    mgr.add_window(&doc);
    w.layout();

    MessageDialog dialog(a);
    dialog.set_texts("Quit", "Really quit?", "Unsaved changes are lost.", "Quit", "Stay");
    Closed result;
    struct Answer { bool accepted = false; int calls = 0; static void fn(void* c, Control& s) { auto* x = static_cast<Answer*>(c); ++x->calls; x->accepted = static_cast<MessageDialog&>(s).accepted(); } } answer;
    dialog.on_result({&Answer::fn, &answer});
    CHECK(dialog.show(mgr) && dialog.showing() && dialog.modal() && mgr.has_modal());
    w.layout();
    // Centered horizontally; minimize and maximize are disabled, close is not.
    CHECK(dialog.bounds().x == (800 - MessageDialog::kWidth) / 2 && !dialog.minimize_enabled() && !dialog.maximize_enabled() && dialog.close_enabled());
    // The modal blocks the window under it: no input, no activation, no focus.
    CHECK(!doc.enabled() && mgr.active() == &dialog && mgr.stacking_index(&dialog) == 1);
    click_center(w, save);
    CHECK(saves.count == 0 && mgr.active() == &dialog && mgr.stacking_index(&dialog) == 1);
    const Point dpart = at_part(doc, Frame::Part::Close);
    click(w, dpart.x, dpart.y);
    CHECK(doc.manager() == &mgr);
    // Disabled dialog buttons do nothing.
    const Point dmin = at_part(dialog, Frame::Part::Minimize);
    click(w, dmin.x, dmin.y);
    CHECK(dialog.state() == Frame::State::Normal && dialog.visible());
    // The default button has the focus; Tab walks only the dialog's buttons.
    CHECK(w.focus() == &dialog.accept_button());
    press(w, key::kTab);
    CHECK(w.focus() == &dialog.decline_button());
    press(w, key::kTab);
    CHECK(w.focus() == &dialog.accept_button());
    // Choosing "Stay" closes the dialog with accepted() false and re-enables the document.
    click_center(w, dialog.decline_button());
    CHECK(!dialog.showing() && answer.calls == 1 && !answer.accepted && !mgr.has_modal() && doc.enabled() && mgr.active() == &doc);
    click_center(w, save);
    CHECK(saves.count == 1);
    // Reopen: the close button counts as "no", Escape too, Enter and the default button as "yes".
    dialog.show(mgr);
    w.layout();
    const Point x = at_part(dialog, Frame::Part::Close);
    click(w, x.x, x.y);
    CHECK(!dialog.showing() && answer.calls == 2 && !answer.accepted);
    dialog.show(mgr);
    press(w, key::kEscape);
    CHECK(!dialog.showing() && answer.calls == 3 && !answer.accepted);
    dialog.show(mgr);
    press(w, key::kEnter);
    CHECK(!dialog.showing() && answer.calls == 4 && answer.accepted);
    dialog.show(mgr);
    w.layout();
    click_center(w, dialog.accept_button());
    CHECK(!dialog.showing() && answer.calls == 5 && answer.accepted);
    // Showing it twice just activates it.
    dialog.show(mgr);
    CHECK(dialog.show(mgr) && mgr.window_count() == 2);
    w.render();
    CHECK(screen.px(dialog.bounds().x + 8, dialog.bounds().y + 8) == theme.title);   // active
    CHECK(screen.px(doc.bounds().x + 8, doc.bounds().y + 8) == theme.inactive_title);  // blocked windows look inactive
    mgr.remove_window(&dialog);
    mgr.remove_window(&doc);
  }
  CHECK(a.live == 0);
}

static void test_notice_dialog() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    // Wrapping: whole words, at most four lines, "..." when text is left out, long words broken.
    {
      String storage(a);
      StringView lines[NoticeDialog::kMaxLines];
      CHECK(NoticeDialog::wrap("short", lines, 4, &storage) == 1 && lines[0].equals("short"));
      CHECK(NoticeDialog::wrap("", lines, 4, &storage) == 0);
      CHECK(NoticeDialog::wrap("Cannot save /documents/report.txt: the drive is full or write protected", lines, 4, &storage) == 2);
      CHECK(lines[0].equals("Cannot save /documents/report.txt: the") && lines[1].equals("drive is full or write protected"));
      CHECK(NoticeDialog::wrap("one\ntwo", lines, 4, &storage) == 2 && lines[0].equals("one") && lines[1].equals("two"));
      String longword(a);
      for (int i = 0; i < 50; ++i) longword.push_back('x');
      CHECK(NoticeDialog::wrap(longword.view(), lines, 4, &storage) == 2 && lines[0].size() == 40 && lines[1].size() == 10);
      String huge(a);
      for (int i = 0; i < 40; ++i) huge.append("word ");
      CHECK(NoticeDialog::wrap(huge.view(), lines, 4, &storage) == 4 && lines[3].size() <= 40 && lines[3].substr(lines[3].size() - 3).equals("..."));
    }
    Frame doc(a);
    doc.set_title("Document");
    doc.set_bounds({50, 50, 300, 200});
    mgr.add_window(&doc);
    NoticeDialog notice(a);
    struct Dismissed { int calls = 0; static void fn(void* c, Control&) { ++static_cast<Dismissed*>(c)->calls; } } dismissed;
    notice.on_dismissed({&Dismissed::fn, &dismissed});
    CHECK(!NoticeDialog::post(nullptr, &mgr, NoticeKind::Error, "x") && !NoticeDialog::post(&notice, nullptr, NoticeKind::Error, "x"));
    CHECK(NoticeDialog::post(&notice, &mgr, NoticeKind::Error, "Cannot open /a: not found") && notice.showing() && notice.modal() && mgr.has_modal());
    w.layout();
    CHECK(notice.title().equals("Error") && notice.kind() == NoticeKind::Error && notice.line_label(0).text().equals("Cannot open /a: not found"));
    CHECK(!notice.minimize_enabled() && !notice.maximize_enabled() && !doc.enabled() && w.focus() == &notice.ok_button());
    // A second notice while it is open replaces the text.
    CHECK(notice.show(mgr, NoticeKind::Warning, "Be careful") && mgr.window_count() == 2);
    CHECK(notice.title().equals("Warning") && notice.line_label(0).text().equals("Be careful"));
    w.render();
    // OK dismisses it and re-enables what was under it.
    click_center(w, notice.ok_button());
    CHECK(!notice.showing() && dismissed.calls == 1 && !mgr.has_modal() && doc.enabled());
    // Enter and Escape dismiss it too.
    CHECK(notice.show(mgr, NoticeKind::Information, "Done") && notice.title().equals("Information"));
    press(w, key::kEnter);
    CHECK(!notice.showing() && dismissed.calls == 2);
    CHECK(notice.show(mgr, NoticeKind::Error, "Again"));
    press(w, key::kEscape);
    CHECK(!notice.showing() && dismissed.calls == 3);
    // A message too long for four lines gets details to scroll through, which fold away, and can be copied whole.
    {
      CHECK(notice.show(mgr, NoticeKind::Information, "Short") && !notice.has_details() && !notice.details_button().visible() && !notice.details_editor().visible());
      notice.close();
      kernel::Clipboard clipboard(a);
      w.set_clipboard(&clipboard);
      String big(a);
      for (int i = 0; i < 60; ++i) {
        big.append("Server stack trace line ");
        big.append_uint(static_cast<uint64_t>(i));
        big.append(" at System.Runtime.AsyncResult.End\n");
      }
      CHECK(notice.show(mgr, NoticeKind::Error, big.view()) && notice.has_details() && notice.details_showing());
      w.layout();
      TextEditor& ed = notice.details_editor();
      CHECK(ed.visible() && notice.details_button().visible() && notice.copy_button().visible() && notice.details_button().text().equals("Hide Details"));
      CHECK(ed.line_count() >= 60 && ed.text().starts_with("Server stack trace line 0 ") && ed.vertical_scroll_bar().maximum_value() > 0);
      CHECK(notice.line_label(3).text().ends_with("..."));
      const Rect2D dialog = notice.bounds();
      CHECK(dialog.width == static_cast<uint32_t>(NoticeDialog::kWideWidth) && dialog.y + static_cast<int32_t>(dialog.height) <= 600);
      // The text scrolls.
      w.mouse_wheel(ed.window_origin().x + 50, ed.window_origin().y + 50, -1);
      CHECK(ed.top_line() > 0);
      w.render();
      // Copy puts the whole message on the clipboard, not what shows.
      click_center(w, notice.copy_button());
      String got(a);
      CHECK(clipboard.get_text(&got) && got.view().equals(big.view()));
      // Hide Details folds the area away; Show Details brings it back.
      click_center(w, notice.details_button());
      CHECK(!notice.details_showing() && !ed.visible() && notice.details_button().text().equals("Show Details") && notice.bounds().height < dialog.height);
      w.render();
      click_center(w, notice.details_button());
      CHECK(notice.details_showing() && ed.visible() && notice.bounds().height == dialog.height);
      // A long line with no break is cut to the width of the box; a new short message takes the details away again.
      String line(a);
      for (int i = 0; i < 400; ++i) line.push_back(i % 9 == 8 ? ' ' : 'x');
      CHECK(notice.show(mgr, NoticeKind::Error, line.view()) && notice.has_details() && ed.line_count() >= 6 && ed.line_length(0) <= static_cast<uint32_t>(NoticeDialog::kDetailColumns));
      CHECK(notice.show(mgr, NoticeKind::Error, "ok again") && !notice.has_details() && !ed.visible() && !notice.details_button().visible() && notice.bounds().width == static_cast<uint32_t>(NoticeDialog::kWidth));
      w.layout();
      w.render();
      click_center(w, notice.ok_button());
      CHECK(!notice.showing());
      w.set_clipboard(nullptr);
    }
    mgr.remove_window(&doc);
  }
  CHECK(a.live == 0);
}

static void test_desktop_toasts() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    CHECK(d.toasts().count() == 0);
    // A program posts a toast; it sits over the windows in the corner above the taskbar, and the desktop's clock takes it away again.
    const uint32_t id = d.notify("Copy finished", "2 files copied", NoticeKind::Information, 3000);
    CHECK(id != 0 && d.toasts().count() == 1);
    d.step(1000000);
    const Rect2D b = d.toasts().toast_bounds(0);
    const Point origin = d.toasts().parent()->window_origin();
    CHECK(origin.x + b.x + static_cast<int32_t>(b.width) == 800 - ToastCenter::kMargin);
    CHECK(origin.y + b.y + static_cast<int32_t>(b.height) <= 600 - desktop::Desktop::kTaskbarHeight);
    CHECK(d.window().hit_test(origin.x + b.x + 10, origin.y + b.y + 10) == &d.toasts());
    d.step(3999999);
    CHECK(d.toasts().showing(id));
    d.step(4000000);
    CHECK(d.toasts().count() == 0);
  }
  CHECK(a.live == 0);
}

static void test_switcher() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    Window& w = d.window();
    WindowManager& mgr = d.windows();
    WindowSwitcher& sw = d.switcher();
    CHECK(sw.button_count() == 0);
    Frame f1(a), f2(a), f3(a);
    f1.set_title("One"); f2.set_title("Two"); f3.set_title("Three");
    f1.set_bounds({20, 20, 300, 200}); f2.set_bounds({60, 60, 300, 200}); f3.set_bounds({100, 100, 300, 200});
    mgr.add_window(&f1); mgr.add_window(&f2); mgr.add_window(&f3);
    d.step(0);
    CHECK(sw.button_count() == 3 && sw.frame_at(0) == &f1 && sw.frame_at(2) == &f3);
    // The switcher fills the taskbar between Start and the clock; each button has a share of it.
    CHECK(sw.bounds().x > d.start_button().bounds().x + static_cast<int32_t>(d.start_button().bounds().width));
    CHECK(sw.bounds().x + static_cast<int32_t>(sw.bounds().width) <= static_cast<int32_t>(d.start_button().parent()->bounds().width));
    const Rect2D b0 = sw.button_rect(0), b1 = sw.button_rect(1);
    CHECK(b0.width == b1.width && b1.x > b0.x + static_cast<int32_t>(b0.width) - 1 && b0.width <= WindowSwitcher::kMaxButtonWidth);
    auto press_button = [&](uint32_t i) {
      const Point o = sw.window_origin();
      const Rect2D r = sw.button_rect(i);
      click(w, o.x + r.x + static_cast<int32_t>(r.width) / 2, o.y + r.y + static_cast<int32_t>(r.height) / 2);
    };
    // Clicking another window's button raises it; clicking the active one's button minimizes it;
    // clicking a minimized one's button restores it.
    CHECK(mgr.active() == &f3);
    press_button(0);
    CHECK(mgr.active() == &f1 && mgr.stacking_index(&f1) == 2);
    press_button(0);
    CHECK(f1.state() == Frame::State::Minimized && mgr.active() == &f3);
    press_button(0);
    CHECK(f1.state() == Frame::State::Normal && mgr.active() == &f1);
    // Maximized windows fill the area above the taskbar, never the taskbar itself.
    f2.maximize();
    CHECK(f2.bounds().height + static_cast<uint32_t>(desktop::Desktop::kTaskbarHeight) == 600 && f2.bounds().width == 800);
    // Dialogs are not on the taskbar, and the switcher is blocked while one is open.
    d.ask_shut_down();
    d.step(1);
    CHECK(sw.button_count() == 3 && d.shutdown_dialog().showing());
    const Frame* before = mgr.active();
    press_button(0);
    CHECK(mgr.active() == before);
    d.shutdown_dialog().close();
    CHECK(!d.shutdown_dialog().showing() && !d.shutdown_requested());
    // The Start menu's About window is an ordinary window and appears on the taskbar once.
    d.open_about();
    d.open_about();
    CHECK(sw.button_count() == 4 && d.about_window().manager() == &mgr);
    d.about_window().minimize();
    d.open_about();
    CHECK(d.about_window().state() != Frame::State::Minimized && mgr.active() == &d.about_window());
    d.step(2);
    // Rendering the whole desktop with windows must not touch memory outside the screen (ASAN).
    w.render();
    mgr.remove_window(&f1); mgr.remove_window(&f2); mgr.remove_window(&f3); mgr.remove_window(&d.about_window());
  }
  CHECK(a.live == 0);
}

struct FakeClock {
  desktop::DateTime now;
  bool ok = true;
  int reads = 0;
  static bool read(void* context, desktop::DateTime* out) {
    auto* c = static_cast<FakeClock*>(context);
    ++c->reads;
    if (c->ok) *out = c->now;
    return c->ok;
  }
};

static void test_clock() {
  char text[24];
  auto fmt = [&](uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi) {
    desktop::DateTime t; t.year = y; t.month = mo; t.day = d; t.hour = h; t.minute = mi;
    return desktop::format_date_time(t, text) == 19 && text[19] == 0;
  };
  CHECK(fmt(2026, 10, 16, 18, 5) && strcmp(text, "10/16/2026 06:05 PM") == 0);
  CHECK(fmt(2026, 1, 2, 0, 0) && strcmp(text, "01/02/2026 12:00 AM") == 0);   // midnight
  CHECK(fmt(2026, 1, 2, 12, 30) && strcmp(text, "01/02/2026 12:30 PM") == 0);  // noon
  CHECK(fmt(2026, 12, 31, 23, 59) && strcmp(text, "12/31/2026 11:59 PM") == 0);
  CHECK(fmt(2026, 6, 9, 11, 7) && strcmp(text, "06/09/2026 11:07 AM") == 0);
  CHECK(fmt(1999, 3, 4, 1, 9) && strcmp(text, "03/04/1999 01:09 AM") == 0);

  TestAllocator a;
  {
    Screen screen(a, 640, 480);
    desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
    CHECK(d.clock_text().equals("--/--/---- --:-- --"));
    FakeClock clock;
    clock.now.year = 2026; clock.now.month = 10; clock.now.day = 16; clock.now.hour = 18; clock.now.minute = 5;
    d.set_clock_source(FakeClock::read, &clock);
    d.step(0);
    CHECK(d.clock_text().equals("10/16/2026 06:05 PM") && clock.reads == 1);
    d.step(500000);                       // same second: the source is not read again
    CHECK(clock.reads == 1);
    clock.now.minute = 6;
    d.step(1000000);                      // next second: read, minute changed
    CHECK(clock.reads == 2 && d.clock_text().equals("10/16/2026 06:06 PM"));
    clock.ok = false;                     // a failing source shows placeholders
    d.step(2000000);
    CHECK(d.clock_text().equals("--/--/---- --:-- --"));
    clock.ok = true; clock.now.hour = 0; clock.now.minute = 0; clock.now.day = 17;
    d.step(3000000);
    CHECK(d.clock_text().equals("10/17/2026 12:00 AM"));
    // The text fits the tray: drawn without clipping the last character (the label is 19 glyphs wide).
    CHECK(d.window().hit_test(640 - 8, 480 - 10) != nullptr);
  }
  CHECK(a.live == 0);
}

// Two monitors: the taskbar on the primary one, windows maximize, dock and open on the monitor they are on, dialogs open where you are working,
// the pointer stays on the monitors, and a monitor pulled out takes its windows to the one that is left.
static void test_desktop_monitors() {
  TestAllocator a;
  {
    using namespace fakemon;
    FakeSource src;
    src.add("DP-1", dell_edid());
    src.add("HDMI-1", small_edid(), true, hardware::ConnectorType::Hdmi);
    FakeGpu gpu;
    gpu.source = &src;
    hardware::SystemMonitors sm(&gpu, a);
    sm.initialize();
    CHECK(sm.apply() && sm.desktop_width() == 4480 && sm.desktop_height() == 1440);
    Screen screen(a, 4480, 1440);   // (the surface the display now has)
    FakeKeyboard kb;
    FakeMouse mouse;
    desktop::Desktop d(a, screen.renderer, &kb, &mouse);
    d.set_monitors(&sm);
    gui::Window& w = d.window();
    gui::WindowManager& m = d.windows();
    w.layout();
    const int32_t bar = desktop::Desktop::kTaskbarHeight;
    CHECK(m.area_count() == 2 && m.area(0).x == 0 && m.area(0).width == 2560 && m.area(0).height == 1440 - static_cast<uint32_t>(bar));
    CHECK(m.area(1).x == 2560 && m.area(1).width == 1920 && m.area(1).height == 1080);
    // The taskbar is at the bottom of the primary monitor only.
    CHECK(d.start_button().window_origin().x < 20 && d.start_button().window_origin().y >= 1440 - bar && d.start_button().window_origin().y < 1440);
    CHECK(w.hit_test(3000, 1070) != nullptr && w.hit_test(3000, 1070) != &d.start_button());

    // A window opens on the primary monitor, maximizes to its work area, and moves and maximizes on the other one.
    desktop::FileExplorer* e = d.open_explorer();
    gui::Frame* host = e->host();
    CHECK(host && host->bounds().x < 2560);
    host->maximize();
    CHECK(host->bounds().x == 0 && host->bounds().y == 0 && host->bounds().width == 2560 && host->bounds().height == 1440 - static_cast<uint32_t>(bar));
    host->maximize();   // back
    host->set_bounds({2700, 100, 600, 400});
    host->maximize();
    CHECK(host->bounds().x == 2560 && host->bounds().y == 0 && host->bounds().width == 1920 && host->bounds().height == 1080);
    host->maximize();
    CHECK(host->bounds().x == 2700 && host->bounds().width == 600);
    host->dock(gui::Frame::Dock::Left);
    CHECK(host->bounds().x == 2560 && host->bounds().width == 960 && host->bounds().height == 1080);
    host->dock(gui::Frame::Dock::Bottom);
    CHECK(host->bounds().x == 2560 && host->bounds().y == 540 && host->bounds().width == 1920 && host->bounds().height == 540);
    host->dock(gui::Frame::Dock::Bottom);   // (back to where it was)
    CHECK(host->bounds().x == 2700);

    // New windows and dialogs come up on the monitor of the window in use.
    d.open_paint();
    CHECK(d.paint().host() && d.paint().host()->bounds().x >= 2560);
    d.ask_shut_down();
    CHECK(d.shutdown_dialog().bounds().x >= 2560 && d.shutdown_dialog().bounds().x < 4480);
    d.shutdown_dialog().close();

    // The pointer goes from one monitor to the other, and stops at the edge of the picture where no monitor is.
    CHECK(d.pointer().x == 2240 && d.pointer().y == 720);
    mouse.move(800, 0, 0);
    mouse.move(0, 500, 0);
    d.pump(0);
    CHECK(d.pointer().x == 3040 && d.pointer().y == 1079);
    mouse.move(-1000, 0, 0);
    d.pump(0);
    CHECK(d.pointer().x == 2040 && d.pointer().y == 1079);   // back onto the first monitor, still at that height

    // Display settings: the two monitors in the picture, their details and lists; dragging one changes the places and the work areas; another primary moves the taskbar.
    d.open_display_settings();
    desktop::DisplaySettings& ds = d.display_settings();
    CHECK(ds.map().box_count() == 2 && ds.selected_monitor() && ds.selected_monitor()->id() == 0 && ds.map().selected() == 0);
    String line(a);
    line.assign(ds.info_line(1).text());
    CHECK(line.view().find(StringView("Dell")) != StringView::npos);
    CHECK(ds.info_line(2).text().find(StringView("27.1 inch")) != StringView::npos || ds.info_line(2).text().find(StringView("27.2 inch")) != StringView::npos);
    CHECK(ds.resolution_list().item_count() >= 2 && ds.scale_list().item_count() >= 1 && !ds.primary_button().enabled());
    CHECK(ds.move_monitor(1, -1920, 0));   // dropped to the left of the first one
    CHECK(sm.layout().find(1) >= 0 && sm.layout().at(static_cast<uint32_t>(sm.layout().find(1))).rect.x == -1920 + 1920);
    ds.map().select(1);
    CHECK(ds.selected_monitor()->id() == 1 && ds.primary_button().enabled());
    CHECK(ds.make_primary());
    CHECK(sm.layout().primary()->id == 1 && sm.layout().primary()->rect.height == 1080);
    CHECK(!ds.primary_button().enabled());
    const hardware::MonitorSlot* prim = sm.layout().primary();
    CHECK(d.start_button().window_origin().x >= prim->rect.x && d.start_button().window_origin().y < 1080);

    // Apply asks "keep?": reverting, keeping, and the 15 seconds running out.
    ds.map().select(0);
    const uint32_t first_width = ds.selected_monitor()->current_mode().width;
    ds.resolution_list().set_selected_index(ds.resolution_list().selected_index() == 0 ? 1 : 0);
    CHECK(ds.apply_settings() && ds.asking() && ds.selected_monitor()->current_mode().width != first_width);
    ds.revert_changes();
    CHECK(!ds.asking() && ds.selected_monitor()->current_mode().width == first_width);
    ds.resolution_list().set_selected_index(ds.resolution_list().selected_index() == 0 ? 1 : 0);
    CHECK(ds.apply_settings() && ds.asking());
    ds.keep_changes();
    CHECK(!ds.asking() && ds.selected_monitor()->current_mode().width != first_width);
    ds.resolution_list().set_selected_index(ds.resolution_list().selected_index() == 0 ? 1 : 0);
    const uint32_t kept_width = ds.selected_monitor()->current_mode().width;
    CHECK(ds.apply_settings() && ds.asking() && ds.seconds_left() == 15);
    for (int i = 0; i < 14; ++i) ds.tick();
    CHECK(ds.asking() && ds.seconds_left() == 1);
    ds.tick();
    CHECK(!ds.asking() && ds.selected_monitor()->current_mode().width == kept_width);

    // A monitor pulled out: its windows go to the one that is left, and the pointer can no longer go there.
    gui::Frame* paint_host = d.paint().host();
    paint_host->set_bounds({2300, 200, 500, 300});   // on the Dell now at x 1920.. (second slot)
    src.connectors[0].info.connected = false;
    src.changed = true;
    d.pump(1000000);
    CHECK(m.area_count() == 1 && sm.layout().enabled_count() == 1);
    m.layout();
    CHECK(paint_host->bounds().x + static_cast<int32_t>(paint_host->bounds().width) > 0 && paint_host->bounds().x < 1920);
    mouse.move(3000, 0, 0);
    d.pump(1000000);
    CHECK(d.pointer().x == 1919);
    // Plugged in again: it is back.
    src.connectors[0].info.connected = true;
    src.changed = true;
    d.pump(2000000);
    CHECK(m.area_count() == 2 && sm.layout().enabled_count() == 2);
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
    CHECK(!d.step(1000000));  // no clock source: nothing to refresh
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
    // It ends with Shut Down: clicking elsewhere first just closes the menu.
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
    kb.push(key::kDown, true); kb.push(key::kDown, false);   // File Explorer
    kb.push(key::kDown, true); kb.push(key::kDown, false);   // Computer Manager
    d.step(5500000);
    kb.push(key::kUp, true); kb.push(key::kUp, false);       // back to File Explorer
    kb.push(key::kUp, true); kb.push(key::kUp, false);       // the selection wraps past the top to the last item, Shut Down... (whatever programs are listed between)
    kb.push(key::kEnter, true); kb.push(key::kEnter, false);
    d.step(6000000);
    // A modal confirmation opens first; nothing is shut down yet.
    CHECK(!start.open() && d.shutdown_dialog().showing() && d.windows().has_modal() && !d.shutdown_requested());
    CHECK(w.focus() == &d.shutdown_dialog().accept_button());
    // Escape declines: the dialog goes away and the desktop keeps running.
    kb.push(key::kEscape, true); kb.push(key::kEscape, false);
    d.step(7000000);
    CHECK(!d.shutdown_dialog().showing() && !d.shutdown_requested() && !d.windows().has_modal());
    // Ask again and accept with Enter on the default button.
    d.ask_shut_down();
    d.step(8000000);
    CHECK(d.shutdown_dialog().showing());
    kb.push(key::kEnter, true); kb.push(key::kEnter, false);
    d.step(9000000);
    CHECK(d.shutdown_requested() && !d.shutdown_dialog().showing());
  }
  CHECK(a.live == 0);
}

// Ctrl+A in a text box selects all of its text, through the whole input path (keyboard events with Ctrl held).
static void test_select_all_from_keyboard() {
  TestAllocator a;
  {
    Theme theme;
    Screen screen(a, 200, 100);
    Window w(a, screen.renderer, theme, {200, 100});
    TextBox box(a), other(a);
    box.set_dock(DockStyle::Top);
    other.set_dock(DockStyle::Top);
    w.add(&box);
    w.add(&other);
    w.layout();
    FakeKeyboard kb;
    InputPump pump(w, &kb, nullptr);
    w.set_focus(&box);
    // A quick chord: Ctrl goes down and up again before the pump reads the events, so the keyboard's own modifier state
    // is already clear when 'A' is delivered - the order of the events is what says Ctrl was held.
    auto ctrl_a = [&](uint8_t ctrl) {
      const uint8_t bits[] = {key::kLeftControl, key::kRightControl, key::kLeftShift};
      for (uint8_t b : bits)
        if (ctrl & (1u << (b - key::kLeftControl))) kb.push(b, true);
      kb.push(key::kA, true);
      kb.push(key::kA, false);
      for (uint8_t b : bits)
        if (ctrl & (1u << (b - key::kLeftControl))) kb.push(b, false);
      pump.pump();
    };
    box.set_text("some text here");
    kb.push(key::kLeft, true); kb.push(key::kLeft, false);
    kb.push(key::kLeft, true); kb.push(key::kLeft, false);
    pump.pump();
    CHECK(box.caret() == 12 && !box.has_selection());
    ctrl_a(hardware::modifier::kLeftControl);           // caret in the middle
    CHECK(box.has_selection() && box.selected_text().equals("some text here") && box.selection_start() == 0 && box.selection_end() == 14);
    CHECK(box.text().equals("some text here"));         // and no 'a' was typed
    kb.push(key::kRight, true); kb.push(key::kRight, false);
    pump.pump();
    CHECK(!box.has_selection() && box.caret() == 14);
    ctrl_a(hardware::modifier::kRightControl);          // the right Ctrl key too
    CHECK(box.selected_text().equals("some text here"));
    ctrl_a(hardware::modifier::kLeftControl | hardware::modifier::kLeftShift);   // extra modifiers do not matter
    CHECK(box.selected_text().equals("some text here"));
    kb.mods = 0;
    kb.push(key::kA, true); kb.push(key::kA, false);    // without Ctrl, 'a' replaces the selection
    pump.pump();
    CHECK(box.text().equals("a") && !box.has_selection());

    // Text wider than the box, with the beginning scrolled out of view; an empty box; the other box is untouched.
    box.set_text("0123456789012345678901234567890123456789012345678901234567890123456789");
    ctrl_a(hardware::modifier::kLeftControl);
    CHECK(box.selected_text().size() == 70 && box.selection_start() == 0);
    w.render();
    box.set_text("");
    ctrl_a(hardware::modifier::kLeftControl);
    CHECK(!box.has_selection() && box.text().empty() && !other.has_selection());
    other.set_text("other");
    w.set_focus(&other);
    ctrl_a(hardware::modifier::kLeftControl);
    CHECK(other.selected_text().equals("other") && !box.has_selection());
  }
  CHECK(a.live == 0);
}

// The Windows programs' windows as the desktop's (Win32Windows): a frame per window, drawn from the program's surface, following what the program does and telling it
// what the user does; the keys go to the program whose window is active, the pointer while it is over one of their pictures (a relative mouse's motion whole; the
// pointer follows their cursor; Ctrl+Alt lets a confined one go).
struct FakeWindow {
  desktop::WindowsWindowInfo info;
  std::string title;
  uint32_t color = 0xFF336699u;
};
struct FakeWindowsSession final : desktop::IWindowsSession {
  explicit FakeWindowsSession(allocators::IAllocator& a) : changes(a) {}
  bool attached = false, confined = false;
  uint32_t width = 0, height = 0;
  desktop::WindowsRect work;
  int32_t border = 0, title_h = 0;
  int32_t cx = 0, cy = 0;
  std::map<uint32_t, FakeWindow> wins;
  std::vector<uint32_t> order;      // bottom first
  List<uint32_t> changes;
  std::vector<uint32_t> repaints;
  std::vector<std::string> log;
  void note(const char* what, int32_t a, int32_t b) { char t[64]; snprintf(t, sizeof t, "%s %d %d", what, a, b); log.push_back(t); }
  void changed(uint32_t w) { changes.push_back(w); }
  void place(uint32_t w, const desktop::WindowsRect& r) {      // the window and its surface (inside the frame when it has a caption)
    FakeWindow& f = wins[w];
    f.info.window = r;
    const int32_t b = f.info.caption ? border : 0, t = f.info.caption ? title_h : 0;
    f.info.surface = {r.x + b, r.y + b + t, r.width - 2 * uint32_t(b), r.height - 2 * uint32_t(b) - uint32_t(t)};
  }
  uint32_t add(uint32_t w, bool caption, const desktop::WindowsRect& r, const char* title, bool active, bool popup = false) {
    FakeWindow& f = wins[w];
    f.info.exists = f.info.visible = true;
    f.info.caption = caption;
    f.info.sizable = f.info.minimize_box = f.info.maximize_box = caption;
    f.info.popup = popup;
    f.title = title;
    place(w, r);
    if (active) for (auto& [k, v] : wins) v.info.active = k == w;
    order.push_back(w);
    changed(w);
    return w;
  }
  void attach(int32_t b, int32_t t) override { attached = true; border = b; title_h = t; }
  void set_display(uint32_t w, uint32_t h, ArrayView<const Monitor> monitors) override {
    width = w;
    height = h;
    monitor_count = uint32_t(monitors.size());
    if (monitors.size() > 0) work = monitors[0].work, scale = monitors[0].scale_percent;
  }
  uint32_t scale = 0;
  // HiDPI: when `sharp`, the program draws at the device's scale (draw_window_scaled, in another colour); the calls are noted
  bool sharp = false;
  uint32_t sharp_color = 0xFFAA5500u;
  std::vector<std::string> sharp_calls;
  bool draw_window_scaled(uint32_t w, graphics::IRenderer2D& device, int32_t x, int32_t y, uint32_t k, uint32_t* width_out, uint32_t* height_out) override {
    if (!sharp) return false;
    auto it = wins.find(w);
    if (it == wins.end()) return false;
    const desktop::WindowsRect& sr = it->second.info.surface;
    device.fill_rect({x, y, sr.width * k / 100, sr.height * k / 100}, sharp_color);
    sharp_calls.push_back(std::to_string(w) + " " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(k));
    *width_out = sr.width;
    *height_out = sr.height;
    return true;
  }
  uint32_t monitor_count = 0;
  uint32_t drag_window = 0, drag_edge = 0;
  uint32_t take_drag(uint32_t* edge) override { const uint32_t w = drag_window; *edge = drag_edge; drag_window = drag_edge = 0; return w; }
  uint32_t menu_window = 0;
  int32_t menu_x = 0, menu_y = 0;
  uint32_t take_window_menu(int32_t* x, int32_t* y) override { const uint32_t w = menu_window; *x = menu_x; *y = menu_y; menu_window = 0; return w; }
  Pointer ptr;
  void pointer(Pointer* out) override { *out = ptr; }
  // cursor 50: a 2 x 2 picture; icon 60: a 16 x 16 red one; anything else a system shape
  bool cursor_picture(uint32_t cursor, uint32_t* system_id, List<uint32_t>* argb, uint32_t* w, uint32_t* h, int32_t* hx, int32_t* hy) override {
    argb->clear();
    *system_id = 0;
    if (cursor == 50) { for (uint32_t i = 0; i < 4; ++i) argb->push_back(0xFF112233u); *w = *h = 2; *hx = 1; *hy = 1; return true; }
    if (cursor == 60) { for (uint32_t i = 0; i < 256; ++i) argb->push_back(0xFFFF0000u); *w = *h = 16; *hx = *hy = 0; return true; }
    *system_id = 32512;
    return true;
  }
  void take_changes(List<uint32_t>* out, List<Repaint>* rep) override {
    out->clear();
    for (uint32_t w : changes.view()) out->push_back(w);
    changes.clear();
    rep->clear();
    for (uint32_t w : repaints) rep->push_back(Repaint{w, {0, 0, 1, 1}});
    repaints.clear();
  }
  void windows(List<uint32_t>* out) override { out->clear(); for (uint32_t w : order) if (wins.count(w)) out->push_back(w); }
  bool window_info(uint32_t w, desktop::WindowsWindowInfo* out, String* title) override {
    auto it = wins.find(w);
    if (it == wins.end()) return false;
    *out = it->second.info;
    if (title) title->assign(StringView(it->second.title.c_str()));
    return true;
  }
  bool draw_window(uint32_t w, graphics::IRenderer2D& r, int32_t x, int32_t y, uint32_t* width_out, uint32_t* height_out) override {
    auto it = wins.find(w);
    if (it == wins.end()) return false;
    const desktop::WindowsRect& sr = it->second.info.surface;
    r.fill_rect({x, y, sr.width, sr.height}, it->second.color);
    *width_out = sr.width;
    *height_out = sr.height;
    return true;
  }
  // the program agrees with whatever the user does
  void move_window(uint32_t w, const desktop::WindowsRect& r) override {
    note("move", r.x, r.y);
    note("size", int32_t(r.width), int32_t(r.height));
    place(w, r);
    changed(w);
  }
  void activate_window(uint32_t w) override {
    note("activate", int32_t(w), 0);
    for (auto& [k, v] : wins) if (v.info.active != (k == w)) { v.info.active = k == w; changed(k); }
  }
  void window_command(uint32_t w, Command c) override {
    note("command", int32_t(w), int32_t(c));
    FakeWindow& f = wins[w];
    if (c == Command::Close) { wins.erase(w); changed(w); return; }
    f.info.minimized = c == Command::Minimize;
    f.info.maximized = c == Command::Maximize;
    if (c == Command::Maximize) place(w, work);
    changed(w);
  }
  void key(uint8_t usage, bool pressed, uint8_t modifiers) override { (void)modifiers; note("key", usage, pressed); }
  void mouse_motion(int32_t dx, int32_t dy) override { note("motion", dx, dy); cx += dx; cy += dy; clamp(); }
  void mouse_position(int32_t x, int32_t y) override { note("position", x, y); cx = x; cy = y; clamp(); }
  void mouse_button(uint32_t button, bool down) override { note("button", int32_t(button), down); }
  bool take_touches = false, take_pens = false;
  bool pen(const desktop::PenInput& p) override {
    if (!take_pens) return false;
    log.push_back("pen " + std::to_string(p.in_range) + " " + std::to_string(p.tip) + " " + std::to_string(p.window) + " " + std::to_string(p.x) + " " +
                  std::to_string(p.y) + " " + std::to_string(p.pressure));
    return true;
  }
  bool touch(hardware::TouchPhase phase, uint32_t id, uint32_t window, int32_t x, int32_t y) override {
    if (phase == hardware::TouchPhase::Down && !take_touches) return false;
    log.push_back("touch " + std::to_string(int(phase)) + " " + std::to_string(id) + " " + std::to_string(window) + " " + std::to_string(x) + " " + std::to_string(y));
    return true;
  }
  void mouse_wheel(int32_t notches) override { note("wheel", notches, 0); }
  // text input: window `text_window` takes composed text while text_on
  bool text_on = false;
  uint32_t text_window = 0;
  bool text_input(TextInputState* out) override {
    *out = TextInputState();
    if (!text_on) return false;
    out->enabled = true;
    out->window = text_window;
    return true;
  }
  bool text_preedit(StringView t, uint32_t cursor) override {
    if (!text_on) return false;
    log.push_back("preedit '" + std::string(t.data() ? t.data() : "", t.size()) + "' " + std::to_string(cursor));
    return true;
  }
  bool text_commit(StringView t) override {
    if (!text_on) return false;
    log.push_back("commit '" + std::string(t.data(), t.size()) + "'");
    return true;
  }
  // drags with the desktop: the program takes the desktop's drag while take_drags; program_drop is a drag a program let go over the desktop
  bool take_drags = false;
  std::string program_drop;
  int32_t program_x = 0, program_y = 0;
  bool desktop_drag(uint32_t w, int32_t x, int32_t y, StringView t) override {
    log.push_back("ddrag " + std::to_string(w) + " " + std::to_string(x) + " " + std::to_string(y) + " " + std::string(t.data() ? t.data() : "", t.size()));
    return take_drags && w != 0;
  }
  bool desktop_drop() override { log.push_back("ddrop"); return take_drags; }
  void desktop_drag_end() override { log.push_back("dend"); }
  bool take_program_drop(String* text, int32_t* x, int32_t* y) override {
    if (program_drop.empty()) return false;
    text->assign(StringView(program_drop.c_str()));
    *x = program_x;
    *y = program_y;
    program_drop.clear();
    return true;
  }
  void program_drop_done(bool taken) override { log.push_back(std::string("program_drop_done ") + (taken ? "1" : "0")); }
  // program_drag: a program's drag over the desktop (not let go yet)
  std::string program_drag_items;
  int32_t drag_x = 0, drag_y = 0;
  bool program_drag(String* text, int32_t* x, int32_t* y) override {
    if (program_drag_items.empty()) return false;
    text->assign(StringView(program_drag_items.c_str()));
    *x = drag_x;
    *y = drag_y;
    return true;
  }
  void program_drag_answer(bool would_take) override { log.push_back(std::string("answer ") + (would_take ? "1" : "0")); }
  void cursor(int32_t* x, int32_t* y) override { *x = cx; *y = cy; }
  bool cursor_confined() override { return confined; }
  uint32_t capture_window() override { return 0; }
  void clamp() {
    if (!confined) return;
    cx = cx < 120 ? 120 : cx > 140 ? 140 : cx;
    cy = cy < 120 ? 120 : cy > 140 ? 140 : cy;
  }
};

// Layers (Frame::Layer): bands of the stack that raising never crosses, and edges windows reserve out of the work areas.
static void test_windows_layers() {
  TestAllocator a;
  Screen screen(a, 800, 600);
  Theme theme;
  Window w(a, screen.renderer, theme, {800, 600});
  WindowManager mgr(a);
  mgr.set_dock(DockStyle::Fill);
  w.add(&mgr);
  w.layout();
  Frame back(a), normal1(a), normal2(a), panel(a), overlay(a);
  back.set_layer(Frame::Layer::Background);
  back.set_decorated(false);
  back.set_activatable(false);
  back.set_bounds({0, 0, 800, 600});
  normal1.set_bounds({50, 50, 300, 200});
  normal2.set_bounds({100, 80, 300, 200});
  panel.set_layer(Frame::Layer::Top);
  panel.set_decorated(false);
  panel.set_activatable(false);
  panel.set_bounds({0, 0, 800, 30});
  overlay.set_layer(Frame::Layer::Overlay);
  overlay.set_decorated(false);
  overlay.set_bounds({300, 200, 200, 100});
  // added in a muddled order: the bands hold whatever came last
  CHECK(mgr.add_window(&panel) && mgr.add_window(&normal1) && mgr.add_window(&back) && mgr.add_window(&overlay) && mgr.add_window(&normal2));
  CHECK(mgr.stacking_index(&back) == 0 && mgr.stacking_index(&normal1) == 1 && mgr.stacking_index(&normal2) == 2 && mgr.stacking_index(&panel) == 3 &&
        mgr.stacking_index(&overlay) == 4);
  CHECK(mgr.active() == &normal2);
  // raising a normal window keeps it under the panel; a click on the background does not bring it up
  mgr.activate(&normal1);
  CHECK(mgr.stacking_index(&normal1) == 2 && mgr.stacking_index(&panel) == 3);
  click(w, 700, 500);
  CHECK(mgr.stacking_index(&back) == 0 && mgr.active() == &normal1);
  CHECK(w.hit_test(400, 10) == &panel && w.hit_test(400, 250) == &overlay);
  // a program's own order (place_next_to) cannot take a window out of its band
  mgr.place_next_to(&normal2, &overlay, true);
  CHECK(mgr.stacking_index(&normal2) <= 2 && mgr.stacking_index(&panel) == 3 && mgr.stacking_index(&overlay) == 4);
  // moving to another band re-sorts
  normal2.set_layer(Frame::Layer::Overlay);
  CHECK(mgr.stacking_index(&normal2) >= 3 && mgr.stacking_index(&panel) == 2);
  normal2.set_layer(Frame::Layer::Normal);
  CHECK(mgr.stacking_index(&panel) == 3);
  // layered windows are placed where they are said to be: not kept inside, not cycled
  panel.set_bounds({0, -5, 800, 30});
  w.layout();
  CHECK(panel.bounds().y == -5);
  panel.set_bounds({0, 0, 800, 30});
  for (int i = 0; i < 4; ++i) {
    mgr.cycle(true);
    CHECK(mgr.active() == &normal1 || mgr.active() == &normal2);
  }
  // reserving: the top panel keeps 30 pixels; maximized windows and the areas leave it out
  const uint32_t serial = mgr.reserve_serial();
  CHECK(mgr.area(0).y == 0 && mgr.area(0).height == 600);
  panel.set_reserve(Frame::Edge::Top, 30);
  CHECK(mgr.reserve_serial() != serial);
  CHECK(mgr.area(0).y == 30 && mgr.area(0).height == 570 && mgr.full_area(0).y == 0 && mgr.full_area(0).height == 600);
  mgr.maximize(&normal1);
  CHECK(normal1.bounds().y == 30 && normal1.bounds().height == 570);
  // a second reservation on another edge, then the panel hidden: the space comes back and the maximized window follows
  Frame dock(a);
  dock.set_layer(Frame::Layer::Bottom);
  dock.set_decorated(false);
  dock.set_bounds({0, 560, 800, 40});
  dock.set_reserve(Frame::Edge::Bottom, 40);
  CHECK(mgr.add_window(&dock));
  CHECK(mgr.area(0).y == 30 && mgr.area(0).height == 530 && normal1.bounds().height == 530);
  CHECK(mgr.stacking_index(&dock) == 1);      // (above the background, below the normal windows)
  mgr.remove_window(&panel);
  CHECK(mgr.area(0).y == 0 && mgr.area(0).height == 560 && normal1.bounds().y == 0 && normal1.bounds().height == 560);
  mgr.minimize(&dock);
  CHECK(mgr.area(0).height == 600);
  mgr.activate(&dock);
  CHECK(mgr.area(0).height == 560);
  // several areas: a reservation counts only on the area it is on
  const Rect2D areas[2] = {{0, 0, 400, 600}, {400, 0, 400, 600}};
  mgr.set_areas(areas, 2);
  CHECK(mgr.area(0).height == 560 && mgr.area(1).height == 560);      // (the dock spans both)
  dock.set_bounds({0, 560, 400, 40});
  CHECK(mgr.area(0).height == 560 && mgr.area(1).height == 600);
  // more than the area: a pixel is left
  dock.set_reserve(Frame::Edge::Bottom, 5000);
  CHECK(mgr.area(0).height == 1 && mgr.area(0).y == 0);
  dock.set_reserve(Frame::Edge::None, 0);
  CHECK(mgr.area(0).height == 600);
  mgr.remove_window(&dock);
  mgr.remove_window(&back);
  mgr.remove_window(&overlay);
  mgr.remove_window(&normal1);
  mgr.remove_window(&normal2);
}

// The input method (Desktop/Win32Windows.cpp): Unicode entry into a window that takes composed text. Ctrl+Shift+U, hex digits, Space puts the character in;
// Backspace takes a digit back, Escape gives up; the keys it takes never reach the program; without a window taking text, the keys are the program's.
static void test_windows_text_input() {
  TestAllocator a;
  Screen screen(a, 800, 600);
  FakeKeyboard kb;
  FakeMouse mouse;
  FakeWindowsSession s(a);
  desktop::Desktop d(a, screen.renderer, &kb, &mouse);
  d.set_windows_session(&s);
  d.step(0);
  s.add(7, true, {100, 80, 200, 150}, "Editor", true);
  d.step(1000);
  auto press = [&](uint8_t usage) { kb.push(usage, true); kb.push(usage, false); };
  auto unicode = [&]() {
    kb.mods = hardware::modifier::kLeftControl | hardware::modifier::kLeftShift;
    kb.push(key::kLeftControl, true);
    kb.push(key::kLeftShift, true);
    press(key::kA + 20);      // U
    kb.push(key::kLeftShift, false);
    kb.push(key::kLeftControl, false);
    kb.mods = 0;
  };
  auto events = [&](const char* prefix) {
    std::vector<std::string> out;
    for (const std::string& e : s.log) if (e.rfind(prefix, 0) == 0) out.push_back(e);
    return out;
  };
  auto keys_of = [&](int usage) {
    int n = 0;
    for (const std::string& e : s.log) if (e.rfind("key " + std::to_string(usage) + " ", 0) == 0) ++n;
    return n;
  };
  // no window takes text: Ctrl+Shift+U is an ordinary key for the program
  s.log.clear();
  unicode();
  d.step(2000);
  CHECK(keys_of(key::kA + 20) == 2 && events("preedit").empty());
  // U+00E9: "u", "ue", "ue9", then Space commits "é"; none of those keys reach the program, the modifiers do
  s.text_on = true;
  s.text_window = 7;
  s.log.clear();
  unicode();
  press(key::kA + 4);      // e
  press(key::k1 + 8);      // 9
  press(key::kSpace);
  d.step(3000);
  std::vector<std::string> pre = events("preedit"), com = events("commit");
  CHECK(pre.size() == 3 && pre[0] == "preedit 'u' 1" && pre[1] == "preedit 'ue' 2" && pre[2] == "preedit 'ue9' 3");
  CHECK(com.size() == 1 && com[0] == "commit '\xc3\xa9'");
  CHECK(keys_of(key::kA + 20) == 0 && keys_of(key::kA + 4) == 0 && keys_of(key::kSpace) == 0 && keys_of(key::kLeftControl) == 2);
  // an emoji (U+1F600), with a digit taken back
  s.log.clear();
  unicode();
  for (uint8_t k : {uint8_t(key::k1), uint8_t(key::kA + 5), uint8_t(key::k1 + 5), uint8_t(key::k1 + 6), uint8_t(key::kBackspace), uint8_t(0x27), uint8_t(0x27)})
    press(k);      // 1 f 6 7 <- 0 0
  press(key::kEnter);
  d.step(4000);
  com = events("commit");
  CHECK(com.size() == 1 && com[0] == "commit '\xf0\x9f\x98\x80'");
  CHECK(keys_of(key::kEnter) == 0 && keys_of(key::kBackspace) == 0);
  // Escape gives up (and is taken); another key gives up and goes on to the program
  s.log.clear();
  unicode();
  press(key::kA + 1);
  press(key::kEscape);
  press(key::kA + 1);      // b, typed now
  d.step(5000);
  pre = events("preedit");
  CHECK(events("commit").empty() && !pre.empty() && pre.back() == "preedit '' 0" && keys_of(key::kEscape) == 0 && keys_of(key::kA + 1) == 2);
  s.log.clear();
  unicode();
  press(key::kA + 2);      // c
  press(key::kA + 22);     // w: not a hex digit
  d.step(6000);
  CHECK(events("commit").empty() && keys_of(key::kA + 22) == 2 && events("preedit").back() == "preedit '' 0");
  // a code point that is no character (a surrogate) is not put in; nothing typed is not either
  s.log.clear();
  unicode();
  for (uint8_t k : {uint8_t(key::kA + 3), uint8_t(key::k1 + 7), uint8_t(0x27), uint8_t(0x27)}) press(k);      // d800
  press(key::kSpace);
  unicode();
  press(key::kSpace);
  d.step(7000);
  CHECK(events("commit").empty());
  // the window stops taking text while composing: the next key is the program's
  s.log.clear();
  unicode();
  press(key::k1);
  d.step(8000);
  s.text_on = false;
  press(key::k1 + 1);
  d.step(9000);
  CHECK(keys_of(key::k1 + 1) == 2 && keys_of(key::k1) == 0);
}

// Drag and drop: a control that starts a drag when the mouse moves with the button down, one that follows drags over it, and one that takes drops.
struct DragSourceControl final : Control {
  explicit DragSourceControl(allocators::IAllocator& a, const char* t) : Control(a), text(t) {}
  const char* text;
  bool down = false;
  int ended = 0;
  void on_mouse_down(const MouseArgs&) override { down = true; }
  void on_mouse_up(const MouseArgs&) override { down = false; }
  void on_mouse_move(const MouseArgs&) override { if (down && window() && !window()->dragging()) window()->begin_drag(this, StringView(text)); }
  void on_drag_ended(bool, int32_t, int32_t) override { ++ended; }
};
struct DragTracker final : Control {
  DragTracker(allocators::IAllocator& a, std::vector<std::string>* l, bool track) : Control(a), log(l), track(track) {}
  std::vector<std::string>* log;
  bool track, accept = false;
  bool tracks_drag() const override { return track; }
  void on_drag_over(StringView t, int32_t x, int32_t y, const Control*) override {
    log->push_back("over " + std::string(t.data(), t.size()) + " " + std::to_string(x) + " " + std::to_string(y));
  }
  void on_drag_leave() override { log->push_back("leave"); }
  bool accepts_drop(StringView, int32_t, int32_t, const Control*) const override { return accept; }
  void on_drop(StringView t, int32_t, int32_t, const Control* source) override {
    log->push_back("drop " + std::string(t.data(), t.size()) + (source ? " from a control" : " from nowhere"));
  }
};

static void test_drag_tracking() {
  TestAllocator a;
  Screen screen(a, 400, 300);
  Theme theme;
  Window w(a, screen.renderer, theme, {400, 300});
  std::vector<std::string> log;
  DragSourceControl source(a, "/Desktop/a.txt");
  DragTracker tracker(a, &log, true), other(a, &log, false);
  source.set_bounds({0, 0, 100, 100});
  tracker.set_bounds({200, 0, 100, 100});
  other.set_bounds({200, 150, 100, 100});
  w.add(&source);
  w.add(&tracker);
  w.add(&other);
  w.layout();
  // over the tracker it hears where; away it hears it left; back, released over it while it takes drops: dropped, no leave
  w.mouse_button(50, 50, MouseButton::Left, true);
  w.mouse_move(55, 50);
  CHECK(w.dragging());
  w.mouse_move(250, 40);
  w.mouse_move(260, 45);
  w.mouse_move(250, 200);
  CHECK((log == std::vector<std::string>{"over /Desktop/a.txt 50 40", "over /Desktop/a.txt 60 45", "leave"}));
  log.clear();
  tracker.accept = true;
  w.mouse_move(210, 10);
  w.mouse_button(210, 10, MouseButton::Left, false);      // (no handler thread: the drop runs at once)
  CHECK((log == std::vector<std::string>{"over /Desktop/a.txt 10 10", "over /Desktop/a.txt 10 10", "drop /Desktop/a.txt from a control"}));
  CHECK(source.ended == 1 && !w.dragging());
  // released over it while it does not take it: it hears the drag left
  log.clear();
  tracker.accept = false;
  w.mouse_button(50, 50, MouseButton::Left, true);
  w.mouse_move(55, 50);
  w.mouse_move(210, 10);
  w.mouse_button(210, 10, MouseButton::Left, false);
  CHECK(!log.empty() && log.back() == "leave");
  // Escape cancels: left too
  log.clear();
  w.mouse_button(50, 50, MouseButton::Left, true);
  w.mouse_move(55, 50);
  w.mouse_move(210, 10);
  KeyArgs esc;
  esc.usage = hardware::key::kEscape;
  w.key(esc);
  CHECK(!w.dragging() && !log.empty() && log.back() == "leave");
  w.mouse_button(210, 10, MouseButton::Left, false);
  // drop_at: a drop with no drag and no source, to whoever takes it
  log.clear();
  tracker.accept = true;
  CHECK(w.drop_at(250, 50, "/Linux/root/x.txt"));
  CHECK(!log.empty() && log.back() == "drop /Linux/root/x.txt from nowhere" && !w.drop_pending());
  CHECK(!w.drop_at(250, 200, "x") && !w.drop_at(250, 50, StringView()));      // (nobody there takes drops; nothing to drop)
  // outside_drag_at: a drag with no source shown over the window; the answer is whether the control there would take it, nothing is dropped
  log.clear();
  CHECK(w.outside_drag_at(250, 50, "/Desktop/b.txt") && w.outside_dragging());
  CHECK((log == std::vector<std::string>{"over /Desktop/b.txt 50 50"}));
  tracker.accept = false;
  CHECK(!w.outside_drag_at(260, 50, "/Desktop/b.txt"));
  CHECK(!w.outside_drag_at(250, 200, "/Desktop/b.txt") && !log.empty() && log.back() == "leave");      // (over a control that does not follow drags)
  tracker.accept = true;
  CHECK(w.outside_drag_at(250, 50, "/Desktop/b.txt", "b.txt (+1)"));
  log.clear();
  w.outside_drag_end();
  CHECK(!w.outside_dragging() && (log == std::vector<std::string>{"leave"}));
  CHECK(!w.outside_drag_at(250, 50, StringView()));
  // the window's own drag wins over one from outside, which is refused while it runs
  CHECK(w.outside_drag_at(250, 50, "/Desktop/b.txt"));
  w.mouse_button(50, 50, MouseButton::Left, true);
  w.mouse_move(55, 50);
  CHECK(w.dragging() && !w.outside_dragging() && !w.outside_drag_at(250, 50, "/Desktop/b.txt"));
  w.mouse_button(250, 50, MouseButton::Left, false);
}

// The desktop's drags over a program's window go to its session (desktop_drag / desktop_drop), never to the program as mouse input; a program's drag let
// go over the desktop is dropped, item by item, on the desktop's window under it.
static void test_windows_drag_bridge() {
  TestAllocator a;
  Screen screen(a, 800, 600);
  FakeKeyboard kb;
  FakeMouse mouse;
  FakeWindowsSession s(a);
  desktop::Desktop d(a, screen.renderer, &kb, &mouse);
  d.set_windows_session(&s);
  gui::WindowManager& wm = d.windows();
  d.step(0);
  const Point mo = wm.window_origin();
  s.add(7, false, {400, 100, 200, 150}, "Program", true);
  std::vector<std::string> log;
  Frame from(a), to(a);
  DragSourceControl source(a, "/Desktop/Documents/a.txt");
  DragTracker sink(a, &log, false);
  sink.accept = true;
  source.set_dock(DockStyle::Fill);
  sink.set_dock(DockStyle::Fill);
  from.add(&source);
  to.add(&sink);
  from.set_bounds({20, 20, 200, 150});
  to.set_bounds({20, 300, 200, 150});
  CHECK(wm.add_window(&from) && wm.add_window(&to));
  d.step(1000);
  // (absolute pointer positions: the screen's pixels in 0..32767)
  auto at = [&](int32_t x, int32_t y, uint8_t buttons) { mouse.at(uint32_t((mo.x + x) * 32768 / 800), uint32_t((mo.y + y) * 32768 / 600), buttons); };
  mouse.absolute = true;
  s.take_drags = true;
  s.log.clear();
  const Rect2D sb = source.bounds();
  const Point sp = source.window_origin();
  const int32_t sx = sp.x - mo.x + int32_t(sb.width) / 2, sy = sp.y - mo.y + int32_t(sb.height) / 2;
  at(sx, sy, 0);
  at(sx, sy, hardware::mouse_button::kLeft);
  at(sx + 12, sy + 3, hardware::mouse_button::kLeft);
  at(500, 170, hardware::mouse_button::kLeft);
  at(505, 172, hardware::mouse_button::kLeft);
  at(505, 172, 0);
  for (int i = 0; i < 6; ++i) d.step(2000 + i * 20);
  std::vector<std::string> drags;
  bool buttons = false;
  for (const std::string& e : s.log) {
    if (e.rfind("ddrag", 0) == 0 || e == "ddrop" || e == "dend") drags.push_back(e);
    if (e.rfind("button", 0) == 0) buttons = true;
  }
  CHECK(drags.size() >= 2 && drags[0].rfind("ddrag 7 ", 0) == 0 && drags[0].find(" /Desktop/Documents/a.txt") != std::string::npos);
  CHECK(!drags.empty() && drags.back() == "ddrop");
  CHECK(!buttons);      // (the program never saw the desktop's drag as its mouse)
  // the program does not take it: the drag over it ends unused
  s.take_drags = false;
  s.log.clear();
  mouse.head = mouse.tail = 0;
  at(sx, sy, 0);
  at(sx, sy, hardware::mouse_button::kLeft);
  at(sx + 12, sy + 3, hardware::mouse_button::kLeft);
  at(500, 170, hardware::mouse_button::kLeft);
  at(500, 170, 0);
  for (int i = 0; i < 6; ++i) d.step(3000 + i * 20);
  bool ended = false, dropped = false;
  for (const std::string& e : s.log) ended |= e == "dend", dropped |= e == "ddrop";
  CHECK(ended && !dropped);
  // a program's drag let go over the desktop's window: each item dropped there; the session told something took it
  const Point tp = sink.window_origin();
  s.program_drop = "/Desktop/Documents/a b.txt\n/Linux/root/x.txt";
  s.program_x = tp.x - mo.x + 30;
  s.program_y = tp.y - mo.y + 30;
  s.log.clear();
  for (int i = 0; i < 8; ++i) d.step(4000 + i * 20);
  CHECK((log == std::vector<std::string>{"drop /Desktop/Documents/a b.txt from nowhere", "drop /Linux/root/x.txt from nowhere"}));
  bool done = false;
  for (const std::string& e : s.log) done |= e == "program_drop_done 1";
  CHECK(done);
  // over nothing that takes it: told so
  s.program_drop = "/x";
  s.program_x = 700;
  s.program_y = 500;
  s.log.clear();
  for (int i = 0; i < 4; ++i) d.step(5000 + i * 20);
  done = false;
  for (const std::string& e : s.log) done |= e == "program_drop_done 0";
  CHECK(done);
  // a program's drop on another program's window: that program's session is asked then (no drag went before it) and takes it
  s.take_drags = true;
  s.program_drop = "/Desktop/Documents/c.txt";
  s.program_x = 450;
  s.program_y = 160;
  s.log.clear();
  log.clear();
  for (int i = 0; i < 4; ++i) d.step(5500 + i * 20);
  {
    bool asked = false, dropped_there = false, told = false;
    for (const std::string& e : s.log) {
      asked |= e.rfind("ddrag 7 ", 0) == 0 && e.find("/Desktop/Documents/c.txt") != std::string::npos;
      dropped_there |= e == "ddrop";
      told |= e == "program_drop_done 1";
    }
    CHECK(asked && dropped_there && told && log.empty());
  }
  s.take_drags = false;
  // a program's drag over the desktop before it is let go: shown on the desktop's window, and the program hears whether what is under it would take it
  gui::Window* dw = wm.window();
  s.program_drag_items = "/Desktop/Documents/a b.txt\n/Linux/root/x.txt";
  s.drag_x = tp.x - mo.x + 30;
  s.drag_y = tp.y - mo.y + 30;
  s.log.clear();
  log.clear();
  for (int i = 0; i < 2; ++i) d.step(6000 + i * 20);
  CHECK(dw && dw->outside_dragging());
  CHECK(!s.log.empty() && s.log.back() == "answer 1");
  bool dropped_early = false;
  for (const std::string& e : log) dropped_early |= e.rfind("drop", 0) == 0;
  CHECK(!dropped_early);
  s.drag_x = 700;
  s.drag_y = 500;
  s.log.clear();
  d.step(6100);
  CHECK(!s.log.empty() && s.log.back() == "answer 0" && dw->outside_dragging());
  s.program_drag_items.clear();
  d.step(6200);
  CHECK(!dw->outside_dragging());
  wm.remove_window(&from);
  wm.remove_window(&to);
}

// Touches over a program's window: a session that takes them gets the contact (with the window and the programs' coordinates) to its end; one that does
// not has the pointer moved and pressed for it; the devices stop emulating a mouse once the desktop reads them.
static void test_windows_touch() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    FakeKeyboard kb;
    FakeMouse mouse;
    FakeTouch touch;
    FakeWindowsSession s(a);
    desktop::Desktop d(a, screen.renderer, &kb, &mouse);
    d.set_windows_session(&s);
    d.set_touch(&touch);
    CHECK(!touch.emulation);
    gui::WindowManager& wm = d.windows();
    d.step(0);
    const Point mo = wm.window_origin();
    s.add(7, true, {100, 80, 200, 150}, "Notepad", true);
    d.step(1000);
    s.log.clear();
    // taken: Down names window 7, Motion and Up follow (also outside the window)
    s.take_touches = true;
    touch.put(hardware::TouchPhase::Down, 1, uint32_t(mo.x + 150), uint32_t(mo.y + 120));
    touch.put(hardware::TouchPhase::Motion, 1, uint32_t(mo.x + 500), uint32_t(mo.y + 120));
    touch.put(hardware::TouchPhase::Up, 1, uint32_t(mo.x + 500), uint32_t(mo.y + 120));
    d.step(2000);
    std::vector<std::string> touches;
    for (const std::string& e : s.log) if (e.rfind("touch", 0) == 0) touches.push_back(e);
    CHECK(touches.size() == 3 && touches[0] == "touch 0 1 7 150 120" && touches[1] == "touch 1 1 0 500 120" && touches[2] == "touch 2 1 0 500 120");
    bool pointer = false;
    for (const std::string& e : s.log) pointer |= e.rfind("button", 0) == 0;
    CHECK(!pointer);      // (no pointer emulation for a taken contact)
    s.log.clear();
    // refused: the pointer goes there and presses, as a mouse would
    s.take_touches = false;
    touch.put(hardware::TouchPhase::Down, 2, uint32_t(mo.x + 150), uint32_t(mo.y + 120));
    touch.put(hardware::TouchPhase::Up, 2, uint32_t(mo.x + 150), uint32_t(mo.y + 120));
    d.step(3000);
    bool pressed = false, released = false;
    for (const std::string& e : s.log) {
      pressed |= e == "button 1 1";
      released |= e == "button 1 0";
    }
    CHECK(pressed && released && d.pointer().x == mo.x + 150 && d.pointer().y == mo.y + 120);
    // on the desktop (no program's window): the desktop's pointer, the session not asked
    s.log.clear();
    s.take_touches = true;
    touch.put(hardware::TouchPhase::Down, 3, uint32_t(mo.x + 600), uint32_t(mo.y + 400));
    touch.put(hardware::TouchPhase::Up, 3, uint32_t(mo.x + 600), uint32_t(mo.y + 400));
    d.step(4000);
    bool asked = false;
    for (const std::string& e : s.log) asked |= e.rfind("touch", 0) == 0;
    CHECK(!asked && d.pointer().x == mo.x + 600);

    // pens: the pen mouse stops once the desktop reads the pens; a session that takes the pen gets it (the pressure scaled to 65535) until it leaves the
    // range; one that does not has the pointer moved by it
    FakePen pens;
    hardware::CompositePen all_pens;
    all_pens.add(&pens);
    hardware::PenMouse pen_mouse(all_pens);
    d.set_pens(&all_pens, &pen_mouse);
    hardware::MouseEvent nothing;
    pens.put(true, false, uint32_t(mo.x + 150), uint32_t(mo.y + 120));
    CHECK(!pen_mouse.poll(&nothing) && pens.events.size() == 1);      // (disabled: it leaves the pen's events alone)
    s.log.clear();
    s.take_pens = true;
    pens.put(true, true, uint32_t(mo.x + 150), uint32_t(mo.y + 120), 0, 500);
    pens.put(true, true, uint32_t(mo.x + 600), uint32_t(mo.y + 120), 0, 1000);      // (outside the window, still the session's)
    pens.put(false, false, uint32_t(mo.x + 600), uint32_t(mo.y + 120));
    d.step(5000);
    std::vector<std::string> pen_log;
    for (const std::string& e : s.log) if (e.rfind("pen", 0) == 0) pen_log.push_back(e);
    CHECK(pen_log.size() == 4 && pen_log[0] == "pen 1 0 7 150 120 0" && pen_log[1] == "pen 1 1 7 150 120 32767" && pen_log[2] == "pen 1 1 0 600 120 65535" &&
          pen_log[3] == "pen 0 0 0 600 120 0");
    CHECK(d.pointer().x == mo.x + 600);      // (the pointer did not follow the pen)
    s.take_pens = false;
    pens.put(true, false, uint32_t(mo.x + 160), uint32_t(mo.y + 130));
    d.step(6000);
    CHECK(d.pointer().x == mo.x + 160 && d.pointer().y == mo.y + 130);
    d.set_windows_session(nullptr);
  }
  CHECK(a.live == 0);
}

// HiDPI: a desktop drawing through a ScaledRenderer2D at 2 tells its programs' session scale 2; the session's window is drawn enlarged by draw_window, or
// at device pixels by draw_window_scaled when the program draws at the scale (its picture's top left at twice the logical place).
static void test_windows_hidpi() {
  TestAllocator a;
  {
    Screen screen(a, 1600, 1200);
    graphics::ScaledRenderer2D scaled(a, screen.renderer, 200);
    FakeKeyboard kb;
    FakeMouse mouse;
    FakeWindowsSession s(a);
    desktop::Desktop d(a, scaled, &kb, &mouse);
    d.set_windows_session(&s);
    gui::WindowManager& wm = d.windows();
    d.step(0);
    CHECK(s.scale == 200 && s.width == wm.bounds().width && wm.bounds().width <= 800);
    const Point mo = wm.window_origin();
    s.add(7, true, {100, 80, 200, 150}, "Notepad", true);
    d.step(1000);
    desktop::Win32Window* h = d.win32_windows().host(7);
    gui::Rect2D pic;
    CHECK(h && h->picture_area(&pic) && pic.x == 106 && pic.y == 105);
    // enlarged: the logical pixel (mo + 110, mo + 110) is the device's 2x2 block there
    CHECK(screen.px((mo.x + 110) * 2, (mo.y + 110) * 2) == 0xFF336699u && screen.px((mo.x + 110) * 2 + 1, (mo.y + 110) * 2 + 1) == 0xFF336699u);
    CHECK(s.sharp_calls.empty());
    // the program draws at the scale: drawn on the device, at twice the place
    s.sharp = true;
    s.changed(7);
    {
      gui::UiGuard lock(&wm);
      h->invalidate();
    }
    d.step(2000);
    CHECK(!s.sharp_calls.empty() && s.sharp_calls.back() == "7 " + std::to_string((mo.x + pic.x) * 2) + " " + std::to_string((mo.y + pic.y) * 2) + " 200");
    CHECK(screen.px((mo.x + 110) * 2, (mo.y + 110) * 2) == 0xFFAA5500u && screen.px((mo.x + pic.x) * 2, (mo.y + pic.y) * 2) == 0xFFAA5500u);
    CHECK(screen.px((mo.x + pic.x) * 2 - 1, (mo.y + pic.y) * 2 + 4) != 0xFFAA5500u);      // (the frame's border, outside the picture)
  }
}

static void test_windows_screen() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    FakeKeyboard kb;
    FakeMouse mouse;
    FakeWindowsSession s(a);
    desktop::Desktop d(a, screen.renderer, &kb, &mouse);
    d.set_windows_session(&s);
    desktop::Win32Windows& ww = d.win32_windows();
    gui::WindowManager& wm = d.windows();
    d.step(0);
    CHECK(s.attached && s.width == wm.bounds().width && s.height == wm.bounds().height && s.border == gui::Frame::kBorder && s.title_h == gui::Frame::kTitleHeight + 1);
    CHECK(s.monitor_count == 1 && s.work.width == wm.area(0).width && s.work.height == wm.area(0).height);
    CHECK(ww.count() == 0);
    const Point mo = wm.window_origin();

    // a program shows a window: a frame of the desktop where the window is, its title, the program's picture inside, the active one
    s.add(7, true, {100, 80, 200, 150}, "Notepad", true);
    d.step(1000);
    desktop::Win32Window* h = ww.host(7);
    CHECK(h && h->manager() == &wm && wm.active() == h && h->title() == StringView("Notepad") && h->decorated() && h->in_taskbar());
    CHECK(h->bounds().x == 100 && h->bounds().y == 80 && h->bounds().width == 200 && h->bounds().height == 150);
    gui::Rect2D pic;
    CHECK(h->picture_area(&pic) && pic.x == 106 && pic.y == 105 && pic.width == 188 && pic.height == 119);      // the program's surface, exactly
    CHECK(screen.px(mo.x + 110, mo.y + 110) == 0xFF336699u && screen.px(mo.x + 150, mo.y + 90) != 0xFF336699u);      // the picture, and the desktop's title bar
    CHECK(s.log.empty());      // (nothing echoed back to the program)

    // the program moves its window: the frame follows, nothing goes back
    s.place(7, {120, 90, 200, 150});
    s.changed(7);
    d.step(2000);
    CHECK(h->bounds().x == 120 && h->bounds().y == 90 && s.log.empty());
    // the user moves the frame: the program is asked
    {
      gui::UiGuard lock(&wm);
      h->set_bounds({140, 100, 220, 160});
    }
    d.step(3000);
    CHECK(s.log.size() == 2 && s.log[0] == "move 140 100" && s.log[1] == "size 220 160");
    d.step(4000);
    CHECK(s.log.size() == 2 && h->bounds().x == 140 && h->bounds().width == 220);      // (the program agreed: no more)
    s.log.clear();

    // a popup (a menu) of the program: no frame, never active, no taskbar button
    s.add(8, false, {150, 120, 60, 40}, "", false, true);
    d.step(5000);
    desktop::Win32Window* menu = ww.host(8);
    CHECK(menu && !menu->decorated() && !menu->activatable() && !menu->in_taskbar() && wm.active() == h && menu->picture_area(&pic) && pic.x == 150 && pic.width == 60);
    CHECK(s.log.empty());
    s.window_command(8, desktop::IWindowsSession::Command::Close);      // (the program takes it down)
    d.step(6000);
    CHECK(ww.host(8) == nullptr && wm.active() == h);
    s.log.clear();

    // keys: the press and the release, while a program's window is the active one
    kb.push(0x04, true); kb.push(0x04, false);
    d.step(7000);
    CHECK(s.log.size() == 2 && s.log[0] == "key 4 1" && s.log[1] == "key 4 0");
    s.log.clear();

    // the pointer comes over the picture: the cursor goes there (screen = the window manager's area), then motion is handed over whole and the pointer follows the cursor
    CHECK(h->picture_area(&pic));
    const Point p = d.pointer();
    mouse.move(mo.x + pic.x + 10 - p.x, mo.y + pic.y + 10 - p.y, 0);
    d.step(8000);
    CHECK(s.log.size() == 1 && s.log[0] == "position 156 135" && ww.grabbed());
    mouse.move(5, -3, 0);
    d.step(9000);
    CHECK(s.log.size() == 2 && s.log[1] == "motion 5 -3" && d.pointer().x == mo.x + 161 && d.pointer().y == mo.y + 132);
    mouse.move(0, 0, hardware::mouse_button::kLeft);
    mouse.move(0, 0, 0);
    d.step(10000);
    CHECK(s.log.size() == 4 && s.log[2] == "button 1 1" && s.log[3] == "button 1 0");
    s.log.clear();
    // a program confines the cursor: the pointer stays with it however far the mouse goes
    s.confined = true;
    mouse.move(-300, 0, 0);
    d.step(11000);
    CHECK(s.log.size() == 1 && s.log[0] == "motion -300 0" && d.pointer().x == mo.x + 120 && ww.grabbed());
    // Ctrl+Alt lets it go until the next click on a program's picture
    s.log.clear();
    kb.push(0xE0, true); kb.push(0xE2, true);
    d.step(12000);
    CHECK(!ww.grabbed() && s.log.size() == 2);      // (the keys went to the program too)
    s.log.clear();
    mouse.move(2, 2, 0);
    d.step(13000);
    CHECK(s.log.empty() && !ww.grabbed());
    kb.push(0xE2, false); kb.push(0xE0, false);
    s.confined = false;
    d.step(14000);
    s.log.clear();
    // not confined: leaving the picture gives the pointer back, nothing is handed over
    mouse.move(mo.x + pic.x + 20 - d.pointer().x, mo.y + pic.y + 20 - d.pointer().y, hardware::mouse_button::kLeft);
    mouse.move(0, 0, 0);
    d.step(15000);
    CHECK(ww.grabbed());
    s.log.clear();
    mouse.move(-400, 0, 0);
    d.step(16000);
    CHECK(s.log.empty() && !ww.grabbed());

    // a desktop window becomes the active one: the program hears it lost the activation; keys stay with the desktop, a release goes where its press went
    kb.push(0x05, true);
    d.step(17000);
    s.log.clear();
    d.open_calculator();
    d.step(18000);
    CHECK(wm.active() != h && s.log.size() == 1 && s.log[0] == "activate 0 0");
    kb.push(0x05, false); kb.push(0x06, true); kb.push(0x06, false);
    d.step(19000);
    CHECK(s.log.size() == 2 && s.log[1] == "key 5 0");
    s.log.clear();
    // ... and the user activates the program's window again
    h->activate();
    d.step(20000);
    CHECK(s.log.size() == 1 && s.log[0] == "activate 7 0" && wm.active() == h);
    s.log.clear();

    // the frame's buttons ask the program: maximize (it fills the work area), minimize, close (the program decides; the frame stays until it destroys the window)
    h->maximize();
    d.step(21000);
    CHECK(s.log.size() == 1 && s.log[0] == "command 7 2" && h->state() == gui::Frame::State::Maximized);
    s.log.clear();
    h->minimize();
    d.step(22000);
    CHECK(s.log.size() == 2 && s.log[0] == "activate 0 0" && s.log[1] == "command 7 1" && h->state() == gui::Frame::State::Minimized);      // (a minimized window is not the active one)
    s.log.clear();
    s.wins[7].info.minimized = false;      // the program restores itself (maximized again)
    s.wins[7].info.maximized = true;
    s.changed(7);
    d.step(23000);
    CHECK(h->state() == gui::Frame::State::Maximized && s.log.empty());
    s.wins[7].color = 0xFF00FF00u;      // the program draws: the frame is drawn again
    s.repaints.push_back(7);
    d.step(24000);
    CHECK(h->picture_area(&pic) && screen.px(mo.x + pic.x + 1, mo.y + pic.y + 1) == 0xFF00FF00u);
    CHECK(wm.active() == h && s.log.size() == 1 && s.log[0] == "activate 7 0");      // (it came back as the active window: the program is told)
    s.log.clear();
    h->close();
    CHECK(h->manager() == &wm && s.log.size() == 1 && s.log[0] == "command 7 0");
    d.step(25000);
    CHECK(ww.host(7) == nullptr && ww.count() == 0);
    d.set_windows_session(nullptr);
  }
  CHECK(a.live == 0);
}

// More of the programs' windows as the desktop's: their stacking order (among the desktop's own windows), a drag a program asks for, the cursor it shows, icons and
// titles outside ASCII.
static void test_windows_more() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    FakeKeyboard kb;
    FakeMouse mouse;
    FakeWindowsSession s(a);
    desktop::Desktop d(a, screen.renderer, &kb, &mouse);
    d.set_windows_session(&s);
    desktop::Win32Windows& ww = d.win32_windows();
    gui::WindowManager& wm = d.windows();
    d.step(0);
    const Point mo = wm.window_origin();
    s.add(7, true, {40, 40, 200, 150}, "Seven", false);
    s.add(8, true, {300, 60, 200, 150}, "Eight", true);
    d.step(1000);
    desktop::Win32Window* h7 = ww.host(7);
    desktop::Win32Window* h8 = ww.host(8);
    CHECK(h7 && h8 && wm.stacking_index(h8) > wm.stacking_index(h7));

    // the program restacks its windows: the frames follow, a desktop window between them keeps its place on top
    d.open_calculator();
    d.step(2000);
    gui::Frame* calc = wm.active();
    s.log.clear();
    s.order = {8, 7};
    s.changed(7);
    d.step(3000);
    CHECK(wm.stacking_index(h7) > wm.stacking_index(h8) && wm.stacking_index(calc) > wm.stacking_index(h7));

    // the user raises the program's window over the desktop's: a window the program keeps above it (an owned one) comes along, above both
    h8->activate();
    d.step(3500);
    CHECK(wm.stacking_index(h8) > wm.stacking_index(calc));
    s.order = {8, 7};
    s.changed(8);
    d.step(3600);
    CHECK(wm.stacking_index(h7) > wm.stacking_index(h8) && wm.stacking_index(h8) > wm.stacking_index(calc));

    // a drag the program asks for (its own title bar): the frame follows the pointer while the left button is held; the program hears of it as a move
    h8->activate();
    d.step(4000);
    gui::Rect2D pic;
    CHECK(h8->picture_area(&pic));
    const Point p0 = d.pointer();
    mouse.move(mo.x + pic.x + 20 - p0.x, mo.y + pic.y + 20 - p0.y, hardware::mouse_button::kLeft);
    d.step(5000);
    s.drag_window = 8;
    s.drag_edge = 9;
    d.step(6000);
    s.log.clear();
    mouse.move(30, 20, hardware::mouse_button::kLeft);
    d.step(7000);
    CHECK(h8->bounds().x == 330 && h8->bounds().y == 80);
    bool moved = false;
    for (const std::string& e : s.log) moved |= e == "move 330 80";
    CHECK(moved);
    mouse.move(0, 0, 0);
    d.step(8000);
    CHECK(s.log.back() == "button 1 0");      // (the press that began it is let go)
    mouse.move(5, 5, 0);
    d.step(9000);
    CHECK(h8->bounds().x == 330);      // (over)
    // ... and a border: the bottom right corner sizes it
    CHECK(h8->picture_area(&pic));
    mouse.move(mo.x + pic.x + 20 - d.pointer().x, mo.y + pic.y + 20 - d.pointer().y, hardware::mouse_button::kLeft);
    d.step(10000);
    s.drag_window = 8;
    s.drag_edge = 8;
    d.step(11000);
    mouse.move(40, 10, hardware::mouse_button::kLeft);
    d.step(12000);
    CHECK(h8->bounds().x == 330 && h8->bounds().width == 240 && h8->bounds().height == 160);
    mouse.move(0, 0, 0);
    d.step(13000);

    // the window menu the program asks for (its own title bar): the frame's, where it asked (client coordinates); Minimize there minimizes the window
    CHECK(h8->window_menu() == nullptr || !h8->window_menu()->open());
    s.menu_window = 8;
    s.menu_x = 15;
    s.menu_y = 4;
    d.step(13100);
    CHECK(h8->window_menu() && h8->window_menu()->open());
    if (h8->window_menu() && d.window().popup()) {
      const gui::Rect2D pop = d.window().popup()->bounds();
      const gui::Point client = h8->view().window_origin();
      CHECK(pop.x == client.x + 15 && pop.y == client.y + 4);
      CHECK(!h8->window_menu()->find_item("Restore")->enabled() && h8->window_menu()->find_item("Close")->enabled());
      d.window().close_popup();
    }
    s.menu_window = 99;      // (no such window: nothing opens)
    d.step(13200);
    CHECK(!d.window().popup());

    // the cursor the program shows over its windows
    CHECK(h8->picture_area(&pic));
    const desktop::Win32WindowView& view = h8->view();
    s.ptr.hidden = true;
    d.step(14000);
    CHECK(view.cursor_shape(0, 0) == gui::CursorShape::Hidden);
    s.ptr.hidden = false;
    s.ptr.system_id = 32513;
    d.step(15000);
    CHECK(view.cursor_shape(0, 0) == gui::CursorShape::IBeam);
    s.ptr.system_id = 32649;
    d.step(16000);
    CHECK(view.cursor_shape(0, 0) == gui::CursorShape::Hand);
    s.ptr.cursor = 50;
    d.step(17000);
    graphics::Surface image;
    Point hot;
    CHECK(view.cursor_shape(0, 0) == gui::CursorShape::Image && view.cursor_image(&image, &hot) && image.width == 2 && hot.x == 1 && image.pixels[3] == 0xFF112233u);
    mouse.move(mo.x + pic.x + 40 - d.pointer().x, mo.y + pic.y + 40 - d.pointer().y, 0);
    d.step(18000);
    const Point at = d.pointer();
    CHECK(screen.px(at.x, at.y) == 0xFF112233u && screen.px(at.x - 1, at.y - 1) == 0xFF112233u);      // (drawn with its hot spot on the pointer)

    // an icon and a title outside ASCII
    s.wins[7].info.icon = 60;
    s.wins[7].title = "Z\xC3\xBC";      // Z, u with diaeresis
    s.changed(7);
    d.step(19000);
    CHECK(h7->has_icon() && h7->title() == StringView("Z\xC3\xBC") && Painter::text_width(h7->title()) == 2 * Painter::kGlyphWidth);
    h7->activate();
    d.step(20000);
    const gui::Rect2D b7 = h7->bounds();
    CHECK(screen.px(mo.x + b7.x + gui::Frame::kBorder + 4, mo.y + b7.y + gui::Frame::kBorder + 4) == 0xFFFF0000u);      // the icon, left in the title bar
    d.set_windows_session(nullptr);
  }
  CHECK(a.live == 0);
}

// The fixed font's characters beyond ASCII: an accented letter is its base letter with the accent; UTF-8 is read a character at a time.
static void test_font_unicode() {
  using hardware::Font8x16;
  uint8_t u[16], ue[16], q[16];
  Font8x16::glyph_of(U'u', u);
  Font8x16::glyph_of(0xFC, ue);      // u with diaeresis
  Font8x16::glyph_of(0x4E00, q);     // (a CJK character: not there)
  const ConstByteView question = Font8x16::glyph('?');
  bool same_below = true, same_q = true;
  for (uint32_t i = 4; i < 16; ++i) same_below &= u[i] == ue[i];
  for (uint32_t i = 0; i < 16; ++i) same_q &= q[i] == question[i];
  CHECK(same_below && ue[3] != 0 && u[3] == 0 && same_q);
  uint8_t i_acute[16];
  Font8x16::glyph_of(0xED, i_acute);      // i acute: the dot gives way to the accent
  CHECK(i_acute[2] != 0 && i_acute[4] == 0);
  const char* text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xC3";
  CHECK(hardware::utf8_next(text) == U'a' && hardware::utf8_next(text) == 0xE9 && hardware::utf8_next(text) == 0x20AC && hardware::utf8_next(text) == 0x1F600);
  CHECK(hardware::utf8_next(text) == 0xFFFD && hardware::utf8_next(text) == 0);      // (cut off)
  CHECK(Painter::text_width("\xC3\xA9t\xC3\xA9") == 3 * Painter::kGlyphWidth);
}

// An X client's window and a Windows program's window on the same desktop: both are frames of the window manager made by the same code (Win32Windows over
// desktop::IWindowsSession), with the same decorations, and the desktop's actions reach the X client as X events.
static void put16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
static void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
static void x11_request(x11::Server& s, uint32_t client, std::vector<uint8_t> b) {      // (b: opcode, data, then the body; the length is filled in)
  while (b.size() % 4) b.push_back(0);
  b[2] = uint8_t(b.size() / 4);
  b[3] = uint8_t(b.size() / 4 >> 8);
  s.receive(client, ConstByteView(b.data(), b.size()));
}

static void test_x11_frames() {
  TestAllocator a;
  {
    Screen screen(a, 800, 600);
    FakeKeyboard kb;
    FakeMouse mouse;
    FakeWindowsSession win32(a);
    x11::Server xs(a);
    desktop::Desktop d(a, screen.renderer, &kb, &mouse);
    d.set_windows_session(&win32);
    d.set_x11_session(&xs);
    gui::WindowManager& wm = d.windows();
    d.step(0);
    const Point mo = wm.window_origin();
    // a Windows program's window
    win32.add(7, true, {40, 40, 200, 150}, "Notepad", false);
    // an X client: setup, a 240x120 window (frame at 300,60) with a title and WM_DELETE_WINDOW, filled red, mapped
    const uint32_t c = xs.open_client(0);
    const uint8_t setup[12] = {'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    xs.receive(c, ConstByteView(setup, 12));
    ByteBuffer out(a);
    xs.take_output(c, &out);
    CHECK(out.size() > 8 && out.view()[0] == 1);
    const uint32_t base = uint32_t(c) << 21, w = base | 1, gc = base | 2, protocols = 69, del = 70;      // (WM_PROTOCOLS, WM_DELETE_WINDOW are the server's first atoms)
    std::vector<uint8_t> b = {1, 0, 0, 0};
    put32(b, w); put32(b, x11::Server::kRootWindow); put16(b, 300); put16(b, 60); put16(b, 240); put16(b, 120); put16(b, 0); put16(b, 1); put32(b, 0);
    put32(b, 0x2 | 0x800); put32(b, 0xFFFFFF); put32(b, (1u << 17) | (1u << 15) | 1u | 4u);
    x11_request(xs, c, b);
    b = {18, 0, 0, 0}; put32(b, w); put32(b, 39); put32(b, 31); b.push_back(8); b.push_back(0); b.push_back(0); b.push_back(0); put32(b, 5);
    for (char ch : std::string("xterm")) b.push_back(uint8_t(ch));
    x11_request(xs, c, b);
    b = {18, 0, 0, 0}; put32(b, w); put32(b, protocols); put32(b, 4); b.push_back(32); b.push_back(0); b.push_back(0); b.push_back(0); put32(b, 1); put32(b, del);
    x11_request(xs, c, b);
    // (WM_NORMAL_HINTS with PPosition: the frame goes where it was asked)
    b = {18, 0, 0, 0}; put32(b, w); put32(b, 40); put32(b, 41); b.push_back(32); b.push_back(0); b.push_back(0); b.push_back(0); put32(b, 18);
    put32(b, 4); for (int i = 0; i < 17; ++i) put32(b, 0);
    x11_request(xs, c, b);
    b = {8, 0, 0, 0}; put32(b, w);
    x11_request(xs, c, b);
    b = {55, 0, 0, 0}; put32(b, gc); put32(b, w); put32(b, 0x4); put32(b, 0xFF0000);
    x11_request(xs, c, b);
    b = {70, 0, 0, 0}; put32(b, w); put32(b, gc); put16(b, 0); put16(b, 0); put16(b, 240); put16(b, 120);
    x11_request(xs, c, b);
    d.step(1000);
    desktop::Win32Window* wh = d.win32_windows().host(7);
    desktop::Win32Window* xh = d.x11_windows().host(w);
    CHECK(wh && xh && xh->manager() == &wm && wh->manager() == &wm);
    CHECK(xh && xh->title() == StringView("xterm") && xh->decorated() && xh->in_taskbar() && wm.active() == xh);
    gui::Rect2D pic;
    CHECK(xh && xh->bounds().x == 300 && xh->bounds().y == 60 && xh->picture_area(&pic) && pic.x == 306 && pic.y == 85 && pic.width == 240 && pic.height == 120);
    CHECK(screen.px(mo.x + 400, mo.y + 140) == 0xFFFF0000u);      // the X client's picture, drawn by the desktop
    // the same decorations: the title bar of an inactive frame looks the same for both kinds (the X one is active now; activate the Windows one and compare)
    {
      gui::UiGuard lock(&wm);
      wm.activate(wh);
    }
    d.step(2000);
    CHECK(wm.active() == wh);
    // (both frames are inactive vs. active here: compare the X frame now inactive with the Windows frame inactive before)
    const uint32_t x_title = screen.px(mo.x + 300 + 8, mo.y + 60 + 8);
    {
      gui::UiGuard lock(&wm);
      wm.activate(xh);
    }
    d.step(3000);
    const uint32_t w_title = screen.px(mo.x + 40 + 8, mo.y + 40 + 8);
    CHECK(x_title == w_title);
    // the desktop activated the X window: the client has the focus
    // the user moves and sizes the frame: the client hears ConfigureNotify with its new inside
    out.clear();
    xs.take_output(c, &out);
    {
      gui::UiGuard lock(&wm);
      xh->set_bounds({320, 70, 300, 200});
    }
    d.step(4000);
    out.clear();
    xs.take_output(c, &out);
    bool configured = false;
    for (uint64_t at = 0; at + 32 <= out.size(); at += 32) {
      ConstByteView e = out.view().subview(at, 32);
      if (e[0] == 1) break;      // (no replies were asked for)
      if (e[0] == 22) configured = (e[16] | e[17] << 8) == 326 && (e[18] | e[19] << 8) == 95 && (e[20] | e[21] << 8) == 288 && (e[22] | e[23] << 8) == 169;
    }
    CHECK(configured);
    d.step(5000);
    CHECK(xh->bounds().x == 320 && xh->bounds().width == 300);
    // closing the frame asks the client (WM_DELETE_WINDOW), which destroys the window: the frame goes
    xh->close();      // (the close button)
    d.step(6000);
    out.clear();
    xs.take_output(c, &out);
    bool asked = false;
    for (uint64_t at = 0; at + 32 <= out.size(); at += 32) {
      ConstByteView e = out.view().subview(at, 32);
      if (e[0] == 33 && (e[12] | e[13] << 8 | e[14] << 16 | uint32_t(e[15]) << 24) == del) asked = true;
    }
    CHECK(asked);
    CHECK(d.x11_windows().host(w) != nullptr);      // (still there until the client agrees)
    b = {4, 0, 0, 0}; put32(b, w);
    x11_request(xs, c, b);
    d.step(7000);
    CHECK(d.x11_windows().host(w) == nullptr && d.x11_windows().count() == 0);
    xs.close_client(c);
    d.set_x11_session(nullptr);
    d.set_windows_session(nullptr);
  }
  CHECK(a.live == 0);
}

// A window's frame tap: it is asked for each frame, and given exactly what was drawn (before present), only when it wants it.
static void test_frame_tap() {
  TestAllocator a;
  Theme theme;
  Screen screen(a, 64, 32);
  Window w(a, screen.renderer, theme, {64, 32});
  Panel fill(a);
  fill.set_dock(DockStyle::Fill);
  w.add(&fill);
  struct Tap final : IFrameTap {
    std::vector<uint32_t> buffer;
    bool want = true;
    int asked = 0, read = 0;
    uint32_t w = 0, h = 0;
    ArrayView<uint32_t> frame_wanted(uint32_t width, uint32_t height) override {
      ++asked;
      if (!want) return ArrayView<uint32_t>();
      buffer.assign(size_t(width) * height, 0xDEADBEEF);
      return ArrayView<uint32_t>(buffer.data(), buffer.size());
    }
    void frame_read(uint32_t width, uint32_t height) override { ++read; w = width; h = height; }
  } tap;
  w.set_frame_tap(&tap);
  w.render();
  CHECK(tap.asked == 1 && tap.read == 1 && tap.w == 64 && tap.h == 32);
  bool same = tap.buffer.size() == 64 * 32;
  for (uint32_t y = 0; y < 32 && same; ++y)
    for (uint32_t x = 0; x < 64 && same; ++x) same = tap.buffer[size_t(y) * 64 + x] == screen.px(int32_t(x), int32_t(y));
  CHECK(same);
  tap.want = false;
  w.invalidate();
  w.render();
  CHECK(tap.asked == 2 && tap.read == 1);
  w.set_frame_tap(nullptr);
  w.render();
  CHECK(tap.asked == 2);
}

int main() {
  test_frame_tap();
  test_dock_layout();
  test_uniform_panel();
  test_button_and_focus();
  test_textbox();
  test_dropdown();
  test_menu();
  test_rendering();
  test_input_pump();
  test_input_pump_touch();
  test_input_pump_pen();
  test_select_all_from_keyboard();
  test_desktop();
  test_desktop_monitors();
  test_clock();
  test_windows();
  test_windows_layers();
  test_windows_text_input();
  test_drag_tracking();
  test_windows_drag_bridge();
  test_modal_and_dialog();
  test_notice_dialog();
  test_switcher();
  test_desktop_toasts();
  test_windows_screen();
  test_windows_hidpi();
  test_windows_touch();
  test_x11_frames();
  test_windows_more();
  test_font_unicode();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gui: all checks passed\n");
  return 0;
}
