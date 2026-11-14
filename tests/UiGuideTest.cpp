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

// Host-side tests of the built-in applications against docs/UserInterfaceGuidelines.md: keyboard access, menus, titles, messages, unsaved changes.
// This file holds what the tests share; the tests are in tests/UiGuide/*.inc, one file per group of applications, each with one function that
// main() calls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "Desktop/Format.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Graphics/Graphics2D/Software/SoftwareRenderer2D.hpp"
#include "Gui/AccessKey.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/CheckBox.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/RadioButton.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Controls/TextEditor.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Window.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"
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
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED %s line %d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

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

[[maybe_unused]] static void press(Window& w, uint8_t usage, uint8_t modifiers = 0) {
  KeyArgs k;
  k.usage = usage; k.modifiers = modifiers; k.character = hardware::key_to_ascii(usage, modifiers);
  w.key(k);
}
// Alt and a letter ('a'..'z'): the access key of a label in the active frame.
[[maybe_unused]] static void alt(Window& w, char letter) { press(w, static_cast<uint8_t>(key::kA + (letter - 'a')), modifier::kLeftAlt); }
// Ctrl and a letter.
[[maybe_unused]] static void ctrl(Window& w, char letter, uint8_t more = 0) { press(w, static_cast<uint8_t>(key::kA + (letter - 'a')), static_cast<uint8_t>(modifier::kLeftControl | more)); }
[[maybe_unused]] static void type_char(Window& w, char c) {
  KeyArgs k;
  k.character = c;
  w.key(k);
}
[[maybe_unused]] static void type_text(Window& w, const char* text) {
  for (; *text; ++text) type_char(w, *text);
}
[[maybe_unused]] static void click(Window& w, int32_t x, int32_t y) {
  w.mouse_move(x, y);
  w.mouse_button(x, y, MouseButton::Left, true);
  w.mouse_button(x, y, MouseButton::Left, false);
}
[[maybe_unused]] static void right_click(Window& w, int32_t x, int32_t y) {
  w.mouse_move(x, y);
  w.mouse_button(x, y, MouseButton::Right, true);
  w.mouse_button(x, y, MouseButton::Right, false);
}
[[maybe_unused]] static Point center(Control& c) {
  const Point o = c.window_origin();
  return {o.x + static_cast<int32_t>(c.bounds().width) / 2, o.y + static_cast<int32_t>(c.bounds().height) / 2};
}
[[maybe_unused]] static void click_center(Window& w, Control& c) { const Point p = center(c); click(w, p.x, p.y); }

// Clicks the row of `item` in the menu shown as the window's popup. False if there is no popup or no such item, or the item is disabled.
[[maybe_unused]] static bool click_popup_item(Window& w, Menu* m, const char* item) {
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
// Chooses a menu item the way a user does: clicks the title, then the item's row in the drop-down.
[[maybe_unused]] static bool choose(Window& w, MenuBar& bar, const char* menu, const char* item) {
  int32_t index = -1;
  for (uint32_t m = 0; m < bar.menu_count(); ++m)
    if (bar.menu(m)->title().equals(menu)) index = static_cast<int32_t>(m);
  if (index < 0 || !bar.find_item(menu, item)) return false;
  bar.open_menu(index);
  return click_popup_item(w, bar.menu(static_cast<uint32_t>(index)), item);
}

// --- What the guidelines ask of every menu bar (section 8), checked the same way for each application.
// The menus that are there come in the standard order: File, Edit, View, the application's own, Window, Help.
[[maybe_unused]] static bool standard_menu_order(MenuBar& bar) {
  auto rank = [](StringView t) { return t.equals("File") ? 0 : t.equals("Edit") ? 1 : t.equals("View") ? 2 : t.equals("Window") ? 4 : t.equals("Help") ? 5 : 3; };
  int last = -1;
  for (uint32_t m = 0; m < bar.menu_count(); ++m) {
    const int r = rank(bar.menu(m)->title());
    if (r < last || (r == last && r != 3)) return false;
    last = r;
  }
  return true;
}
// Every title has an access key of its own once the frame handed them out, and so has every item inside its menu.
[[maybe_unused]] static bool menus_have_access_keys(Frame& frame, MenuBar& bar) {
  frame.assign_access_keys();
  uint32_t titles = 0;
  for (uint32_t m = 0; m < bar.menu_count(); ++m) {
    const char k = AccessKeys::key_of(bar.access_label(m), bar.access_index(m));
    if (!k || (titles & (1u << (k - 'a')))) { printf("  menu title without a key of its own: %.*s\n", static_cast<int>(bar.menu(m)->title().size()), bar.menu(m)->title().data()); return false; }
    titles |= 1u << (k - 'a');
    Menu* menu = bar.menu(m);
    menu->assign_access_keys();
    uint32_t items = 0;
    for (uint32_t i = 0; i < menu->item_count(); ++i) {
      const MenuItem* item = menu->item(i);
      if (item->separator) continue;
      const char ik = AccessKeys::key_of(item->text.view(), item->access_index);
      if (!ik || (items & (1u << (ik - 'a')))) { printf("  menu item without a key of its own: %.*s\n", static_cast<int>(item->text.size()), item->text.data()); return false; }
      items |= 1u << (ik - 'a');
    }
  }
  return true;
}
// The last menu is Help and its last item is "About <application>" (no ellipsis).
[[maybe_unused]] static bool help_menu_ends_in_about(MenuBar& bar, const char* application) {
  if (bar.menu_count() == 0) return false;
  Menu* help = bar.menu(bar.menu_count() - 1);
  if (!help->title().equals("Help") || help->item_count() == 0) return false;
  const std::string wanted = std::string("About ") + application;
  return help->item(help->item_count() - 1)->text.view().equals(wanted.c_str());
}

#include "UiGuide/Toolkit.inc"
#include "UiGuide/Editors.inc"
#include "UiGuide/Files.inc"
#include "UiGuide/Tools.inc"
#include "UiGuide/Settings.inc"
#include "UiGuide/Browser.inc"
#include "UiGuide/Shell.inc"

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not lose the report of the checks that failed before it
  ui_guide_toolkit();
  ui_guide_editors();
  ui_guide_files();
  ui_guide_tools();
  ui_guide_settings();
  ui_guide_browser();
  ui_guide_shell();
  if (g_failed) {
    printf("UiGuideTest: %d checks FAILED\n", g_failed);
    return 1;
  }
  printf("UiGuideTest: all passed\n");
  return 0;
}
