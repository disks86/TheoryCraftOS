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

#include "GuiDemo.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Delay.hpp"
#include "console.hpp"

namespace {
using namespace gui;

struct Counters {
  int new_items = 0, exits = 0, button_clicks = 0, selections = 0, edits = 0;
};

void count(int* counter) { ++*counter; }

void on_new(void* c, Control&) { count(&static_cast<Counters*>(c)->new_items); }
void on_exit(void* c, Control&) { count(&static_cast<Counters*>(c)->exits); }
void on_button(void* c, Control&) { count(&static_cast<Counters*>(c)->button_clicks); }
void on_selection(void* c, Control&) { count(&static_cast<Counters*>(c)->selections); }
void on_edit(void* c, Control&) { count(&static_cast<Counters*>(c)->edits); }

void click_at(Window& w, Point p) {
  w.mouse_move(p.x, p.y);
  w.mouse_button(p.x, p.y, MouseButton::Left, true);
  w.mouse_button(p.x, p.y, MouseButton::Left, false);
}

Point center_of(const Control& c) {
  const Point o = c.window_origin();
  return {o.x + static_cast<int32_t>(c.bounds().width) / 2, o.y + static_cast<int32_t>(c.bounds().height) / 2};
}

void type_text(Window& w, const char* text) {
  for (; *text; ++text) {
    KeyArgs k;
    k.character = *text;
    w.key(k);
  }
}
}  // namespace

bool run_gui_demo(graphics::SystemGraphics& system, allocators::IAllocator& allocator) {
  graphics::IRenderer2D* renderer = system.renderer_2d();
  if (!renderer) return false;
  const graphics::Extent2D screen = renderer->extent();
  Counters counters;
  const Handler h_new{on_new, &counters}, h_exit{on_exit, &counters}, h_button{on_button, &counters};

  const Theme theme;
  Window window(allocator, *renderer, theme, screen);
  window.set_title("TheoryCraftOS GUI toolkit");

  MenuBar menu_bar(allocator);
  Menu* file = menu_bar.add_menu("File");
  Menu* edit = menu_bar.add_menu("Edit");
  Menu* help = menu_bar.add_menu("Help");
  if (!file || !edit || !help) return false;
  file->add_item("New", h_new, "Ctrl+N");
  file->add_item("Open...", {}, "Ctrl+O");
  if (MenuItem* save = file->add_item("Save", {}, "Ctrl+S")) save->set_enabled(false);
  file->add_separator();
  file->add_item("Exit", h_exit, "Alt+F4");
  edit->add_item("Undo", {}, "Ctrl+Z");
  edit->add_item("Redo", {}, "Ctrl+Y");
  edit->add_separator();
  if (MenuItem* wrap = edit->add_item("Word wrap")) wrap->set_checked(true);
  help->add_item("About TheoryCraftOS");

  Panel tool_bar(allocator);
  tool_bar.set_style(PanelStyle::Raised);
  tool_bar.set_dock(DockStyle::Top);
  tool_bar.set_size(0, 34);
  tool_bar.set_padding(Insets::all(4));
  Button run(allocator), stop(allocator), step(allocator);
  run.set_text("Run");
  stop.set_text("Stop");
  step.set_text("Step");
  run.on_click(h_button);
  Button* tools[3] = {&run, &stop, &step};
  for (Button* b : tools) {
    b->set_dock(DockStyle::Left);
    b->set_size(64, 0);
    tool_bar.add(b);
  }

  Label status(allocator);
  status.set_dock(DockStyle::Bottom);
  status.set_size(0, 24);
  status.set_text("Ready");

  Panel side(allocator);
  side.set_style(PanelStyle::Sunken);
  side.set_dock(DockStyle::Left);
  side.set_size(220, 0);
  side.set_padding(Insets::all(8));
  Label name_label(allocator);
  name_label.set_text("Name:");
  TextBox name(allocator);
  name.on_changed({on_edit, &counters});
  Label mode_label(allocator);
  mode_label.set_text("Mode:");
  DropDown mode(allocator);
  mode.add_item("Software");
  mode.add_item("Accelerated");
  mode.add_item("Auto");
  mode.on_selection_changed({on_selection, &counters});
  Control* fields[4] = {&name_label, &name, &mode_label, &mode};
  for (Control* c : fields) {
    c->set_dock(DockStyle::Top);
    side.add(c);
  }

  UniformPanel content(allocator);
  content.set_dock(DockStyle::Fill);
  content.set_padding(Insets::all(8));
  content.set_spacing(8);
  Panel cells[3] = {Panel(allocator), Panel(allocator), Panel(allocator)};
  Button cell_buttons[3] = {Button(allocator), Button(allocator), Button(allocator)};
  static const char* const kCellText[3] = {"One third", "One third", "One third"};
  for (int i = 0; i < 3; ++i) {
    cells[i].set_style(PanelStyle::Sunken);
    cells[i].set_padding(Insets::all(10));
    cell_buttons[i].set_text(kCellText[i]);
    cell_buttons[i].set_dock(DockStyle::Top);
    cell_buttons[i].set_size(0, 28);
    cell_buttons[i].on_click(h_button);
    cells[i].add(&cell_buttons[i]);
    content.add(&cells[i]);
  }

  // Order matters for docking: strips first, the fill area last.
  window.add(&menu_bar);
  menu_bar.set_dock(DockStyle::Top);
  window.add(&tool_bar);
  window.add(&status);
  window.add(&side);
  window.add(&content);
  window.layout();

  // Even split: the three cells differ by at most one pixel and fill the content area.
  const uint32_t total = cells[0].bounds().width + cells[1].bounds().width + cells[2].bounds().width + 2 * 8;
  bool ok = total == content.client_rect().width && cells[0].bounds().width - cells[2].bounds().width <= 1;
  // Docking: the side panel starts under the tool bar and the content area is right of it.
  ok = ok && side.bounds().y == tool_bar.bounds().y + static_cast<int32_t>(tool_bar.bounds().height) &&
       content.bounds().x == static_cast<int32_t>(side.bounds().width);

  // Mouse: click the Run button, then the text box; keyboard: type; mouse: pick from the drop-down.
  click_at(window, center_of(run));
  ok = ok && counters.button_clicks == 1;
  click_at(window, center_of(name));
  type_text(window, "TCOS");
  ok = ok && name.text().equals("TCOS") && counters.edits == 4;
  click_at(window, center_of(mode));
  ok = ok && mode.open();
  const Rect2D list = window.popup() ? window.popup()->bounds() : Rect2D{};
  click_at(window, {list.x + 10, list.y + 2 + DropDownList::kRowHeight + 4});  // the second item
  ok = ok && !mode.open() && mode.selected_index() == 1 && counters.selections == 1;
  // Menu: File > New, then File > Exit.
  Point title = {12, menu_bar.window_origin().y + 8};
  click_at(window, title);
  ok = ok && menu_bar.open_menu_index() == 0 && window.popup();
  if (window.popup()) {
    const Rect2D p = window.popup()->bounds();
    click_at(window, {p.x + 20, p.y + 2 + MenuPopup::kRowHeight / 2});
  }
  ok = ok && counters.new_items == 1 && window.popup() == nullptr;
  status.set_text(ok ? "All GUI checks passed" : "A GUI check failed");

  // Leave a menu open in the picture, then show it for a moment.
  click_at(window, {64, title.y});
  window.render();
  hardware::delay_us(1500000);
  window.close_popup();

  console::Guard guard;
  console::puts("GUI toolkit: ");
  console::puts(ok ? "ok" : "FAILED");
  console::puts(" (menus, buttons, text box, drop-down, docking, even split; ");
  console::puts(renderer->accelerated() ? "GPU-drawn)\n" : "CPU-drawn)\n");
  return ok;
}
