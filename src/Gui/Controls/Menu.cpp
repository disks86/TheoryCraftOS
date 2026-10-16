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

#include "Gui/Controls/Menu.hpp"
#include <new>
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
template <class T>
void destroy(allocators::IAllocator& allocator, T* object) {
  object->~T();
  allocator.deallocate(object);
}
}  // namespace

// --- Menu ----------------------------------------------------------------------

Menu::~Menu() {
  for (uint64_t i = 0; i < items_.count(); ++i) destroy(allocator_, items_[i]);
}

MenuItem* Menu::add(StringView text, StringView shortcut, bool separator, Handler on_click) {
  void* memory = allocator_.allocate(sizeof(MenuItem));
  if (!memory) return nullptr;
  MenuItem* item = new (memory) MenuItem(allocator_);
  item->separator = separator;
  item->on_click = on_click;
  if (!item->text.assign(text) || !item->shortcut.assign(shortcut) || !items_.push_back(item)) {
    destroy(allocator_, item);
    return nullptr;
  }
  return item;
}

MenuItem* Menu::add_item(StringView text, Handler on_click, StringView shortcut) {
  return add(text, shortcut, false, on_click);
}

MenuItem* Menu::add_separator() { return add(StringView(), StringView(), true, {}); }

// --- MenuPopup -----------------------------------------------------------------

Size MenuPopup::default_size() const {
  int32_t text = 0, shortcut = 0, height = 4;
  for (uint32_t i = 0; menu_ && i < menu_->item_count(); ++i) {
    const MenuItem* it = menu_->item(i);
    height += it->separator ? kSeparatorHeight : kRowHeight;
    const int32_t t = Painter::text_width(it->text.view()), s = Painter::text_width(it->shortcut.view());
    if (t > text) text = t;
    if (s > shortcut) shortcut = s;
  }
  int32_t width = 26 + text + (shortcut ? 24 + shortcut : 0) + 14;
  if (width < min_width_) width = min_width_;
  return {width, height};
}

int32_t MenuPopup::row_top(int32_t index) const {
  int32_t y = 2;
  for (int32_t i = 0; i < index; ++i) y += menu_->item(static_cast<uint32_t>(i))->separator ? kSeparatorHeight : kRowHeight;
  return y;
}

int32_t MenuPopup::item_at(int32_t y) const {
  if (!menu_) return -1;
  int32_t top = 2;
  for (uint32_t i = 0; i < menu_->item_count(); ++i) {
    const int32_t h = menu_->item(i)->separator ? kSeparatorHeight : kRowHeight;
    if (y >= top && y < top + h) return static_cast<int32_t>(i);
    top += h;
  }
  return -1;
}

void MenuPopup::paint(Painter& painter) {
  if (!menu_) return;
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.face);
  painter.bevel(all, false);
  for (uint32_t i = 0; i < menu_->item_count(); ++i) {
    const MenuItem* it = menu_->item(i);
    const int32_t top = row_top(static_cast<int32_t>(i)), w = static_cast<int32_t>(bounds_.width);
    if (it->separator) {
      painter.line(4, top + kSeparatorHeight / 2 - 1, w - 5, top + kSeparatorHeight / 2 - 1, t.shadow);
      painter.line(4, top + kSeparatorHeight / 2, w - 5, top + kSeparatorHeight / 2, t.highlight);
      continue;
    }
    const bool hot = static_cast<int32_t>(i) == highlighted_ && it->enabled;
    if (hot) painter.fill({3, top, bounds_.width - 6, static_cast<uint32_t>(kRowHeight)}, t.selection);
    const uint32_t color = !it->enabled ? t.disabled_text : hot ? t.selection_text : t.text;
    const int32_t ty = top + (kRowHeight - Painter::kGlyphHeight) / 2;
    if (it->checked) {  // a check mark
      painter.line(8, top + 9, 10, top + 11, color);
      painter.line(10, top + 11, 15, top + 5, color);
      painter.line(8, top + 10, 10, top + 12, color);
      painter.line(10, top + 12, 15, top + 6, color);
    }
    painter.text(22, ty, it->text.view(), color);
    if (!it->shortcut.empty()) painter.text(w - 10 - Painter::text_width(it->shortcut.view()), ty, it->shortcut.view(), color);
  }
}

void MenuPopup::on_mouse_move(const MouseArgs& args) {
  const int32_t i = item_at(args.y);
  const bool inside = contains({0, 0, bounds_.width, bounds_.height}, args.x, args.y);
  const int32_t now = inside && i >= 0 && !menu_->item(static_cast<uint32_t>(i))->separator ? i : -1;
  if (inside && now != highlighted_) {
    highlighted_ = now;
    invalidate();
  }
}

void MenuPopup::run(int32_t index) {
  if (index < 0) return;
  MenuItem* it = menu_->item(static_cast<uint32_t>(index));
  if (it && it->enabled && !it->separator) host_.activate(it);
}

void MenuPopup::on_mouse_up(const MouseArgs& args) { run(item_at(args.y)); }

void MenuPopup::step(int32_t direction, int32_t from) {
  const int32_t n = static_cast<int32_t>(menu_->item_count());
  if (n == 0) return;
  int32_t i = from;
  for (int32_t tries = 0; tries < n; ++tries) {
    i = (i + direction + n) % n;
    const MenuItem* it = menu_->item(static_cast<uint32_t>(i));
    if (!it->separator && it->enabled) {
      highlighted_ = i;
      invalidate();
      return;
    }
  }
}

bool MenuPopup::on_key(const KeyArgs& args) {
  using namespace hardware;
  switch (args.usage) {
    case key::kDown:
      step(1, highlighted_);
      break;
    case key::kUp:
      step(-1, highlighted_ < 0 ? 0 : highlighted_);
      break;
    case key::kLeft:
      host_.switch_menu(-1);
      break;
    case key::kRight:
      host_.switch_menu(1);
      break;
    case key::kEnter:
    case key::kSpace:
      run(highlighted_);
      break;
    case key::kEscape:
      host_.close_menu();
      break;
    default:
      break;
  }
  return true;
}

// --- MenuBar -------------------------------------------------------------------

MenuBar::~MenuBar() {
  for (uint64_t i = 0; i < menus_.count(); ++i) destroy(allocator(), menus_[i]);
}

Menu* MenuBar::add_menu(StringView title) {
  void* memory = allocator().allocate(sizeof(Menu));
  if (!memory) return nullptr;
  Menu* m = new (memory) Menu(allocator());
  if (!m->title_.assign(title) || !menus_.push_back(m)) {
    destroy(allocator(), m);
    return nullptr;
  }
  invalidate();
  return m;
}

int32_t MenuBar::title_width(int32_t index) const {
  return Painter::text_width(menus_[static_cast<uint64_t>(index)]->title()) + 2 * kTitlePad;
}

int32_t MenuBar::title_x(int32_t index) const {
  int32_t x = 2;
  for (int32_t i = 0; i < index; ++i) x += title_width(i);
  return x;
}

int32_t MenuBar::title_at(int32_t x) const {
  for (int32_t i = 0; i < static_cast<int32_t>(menus_.count()); ++i)
    if (x >= title_x(i) && x < title_x(i) + title_width(i)) return i;
  return -1;
}

void MenuBar::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.face);
  painter.line(0, static_cast<int32_t>(bounds_.height) - 1, static_cast<int32_t>(bounds_.width) - 1,
               static_cast<int32_t>(bounds_.height) - 1, t.shadow);
  for (int32_t i = 0; i < static_cast<int32_t>(menus_.count()); ++i) {
    const Rect2D r{title_x(i), 1, static_cast<uint32_t>(title_width(i)), bounds_.height - 3};
    const bool lit = i == open_;
    if (lit) painter.fill(r, t.selection);
    else if (i == hot_ && open_ < 0) painter.bevel(r, false);
    painter.text_centered(r, menus_[static_cast<uint64_t>(i)]->title(), lit ? t.selection_text : t.text);
  }
}

void MenuBar::open_menu(int32_t index) {
  Window* w = window();
  if (!w || index < 0 || index >= static_cast<int32_t>(menus_.count()) || index == open_) return;
  popup_.show(menus_[static_cast<uint64_t>(index)]);
  const Point o = window_origin();
  if (w->open_popup(&popup_, this, o.x + title_x(index), o.y + static_cast<int32_t>(bounds_.height) - 1))
    open_ = index;  // after open_popup: it closes the previous popup, which resets open_
  invalidate();
}

void MenuBar::close_menu() {
  if (open_ >= 0 && window()) window()->close_popup();
  open_ = -1;
  invalidate();
}

void MenuBar::switch_menu(int32_t direction) {
  const int32_t n = static_cast<int32_t>(menus_.count());
  if (n == 0 || open_ < 0) return;
  open_menu((open_ + direction + n) % n);
  popup_.highlight_first();
}

void MenuBar::activate(MenuItem* item) {
  const Handler handler = item->on_click;
  close_menu();
  handler(*this);
}

void MenuBar::on_mouse_move(const MouseArgs& args) {
  const int32_t i = title_at(args.x);
  if (i != hot_) {
    hot_ = i;
    invalidate();
  }
  if (open_ >= 0 && i >= 0 && i != open_) open_menu(i);  // sliding across the bar while a menu is open
}

void MenuBar::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const int32_t i = title_at(args.x);
  if (i >= 0) open_menu(i);
}

}  // namespace gui
