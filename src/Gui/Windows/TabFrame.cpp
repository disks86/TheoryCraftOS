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

#include "Gui/Windows/TabFrame.hpp"
#include "Gui/Window.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
constexpr const char kDragPayload[] = "\x01tab";
constexpr uint32_t kInactiveTab = 0xFFA8A8A8u;
}

// --- Geometry ---------------------------------------------------------------------------------------

Rect2D TabFrame::strip_rect() const {
  if (fullscreen_) return {0, 0, 0, 0};   // (no strip while a tab is fullscreen)
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder;
  return {kBorder, kBorder + kTitleHeight + 1, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(kStripHeight)};
}

Rect2D TabFrame::client_rect() const {
  Rect2D r = Frame::client_rect();
  if (fullscreen_) return r;
  const uint32_t strip = static_cast<uint32_t>(kStripHeight);
  r.y += kStripHeight;
  r.height = r.height > strip ? r.height - strip : 0;
  return r;
}

int32_t TabFrame::tab_width() const {
  const Rect2D strip = strip_rect();
  const int32_t n = static_cast<int32_t>(tabs_.count());
  if (n == 0) return kMaxTabWidth;
  const int32_t room = static_cast<int32_t>(strip.width) - kNewTabWidth - 6;
  int32_t each = room / n;
  if (each > kMaxTabWidth) each = kMaxTabWidth;
  if (each < kMinTabWidth) each = kMinTabWidth;
  return each;
}

Rect2D TabFrame::tab_rect(uint32_t index) const {
  const Rect2D strip = strip_rect();
  const int32_t w = tab_width();
  return {strip.x + static_cast<int32_t>(index) * w, strip.y + 2, static_cast<uint32_t>(w), static_cast<uint32_t>(kStripHeight - 2)};
}

Rect2D TabFrame::close_rect(uint32_t index) const {
  const Rect2D t = tab_rect(index);
  return {t.x + static_cast<int32_t>(t.width) - kCloseSize - 5, t.y + (static_cast<int32_t>(t.height) - kCloseSize) / 2, static_cast<uint32_t>(kCloseSize),
          static_cast<uint32_t>(kCloseSize)};
}

Rect2D TabFrame::new_tab_button_rect() const {
  const Rect2D strip = strip_rect();
  return {strip.x + static_cast<int32_t>(tabs_.count()) * tab_width() + 4, strip.y + 3, static_cast<uint32_t>(kNewTabWidth),
          static_cast<uint32_t>(kStripHeight - 6)};
}

int32_t TabFrame::tab_at_point(int32_t x, int32_t y) const {
  for (uint32_t i = 0; i < tabs_.count(); ++i)
    if (contains(tab_rect(i), x, y)) return static_cast<int32_t>(i);
  return -1;
}

int32_t TabFrame::index_of(const Frame* tab) const {
  for (uint32_t i = 0; i < tabs_.count(); ++i)
    if (tabs_[i] == tab) return static_cast<int32_t>(i);
  return -1;
}

// --- The tabs ---------------------------------------------------------------------------------------

bool TabFrame::add_tab(Frame* tab, int32_t index) {
  UiGuard lock(this);
  if (!tab || tab == this || tab->host_ == this || (tab->manager_ && !tab->host_)) return false;   // (a window of its own cannot become a tab)
  if (tab->host_) tab->host_->remove_tab(tab);
  if (tab->home_.width == 0) tab->home_ = tab->bounds();
  const uint64_t at = index < 0 || static_cast<uint64_t>(index) > tabs_.count() ? tabs_.count() : static_cast<uint64_t>(index);
  if (!tabs_.insert(at, tab)) return false;
  if (!add(tab)) {
    tabs_.remove_at(at);
    return false;
  }
  tab->host_ = this;
  tab->set_dock(DockStyle::None);
  tab->set_visible(false);
  __atomic_store_n(&tab->manager_, manager_, __ATOMIC_RELEASE);
  select_tab(tab);
  return true;
}

bool TabFrame::remove_tab(Frame* tab) {
  UiGuard lock(this);
  const int32_t index = index_of(tab);
  if (index < 0) return false;
  tabs_.remove_at(static_cast<uint64_t>(index));
  remove(tab);
  tab->host_ = nullptr;
  __atomic_store_n(&tab->manager_, static_cast<WindowManager*>(nullptr), __ATOMIC_RELEASE);
  tab->set_visible(true);
  if (dragging_tab_ == tab) dragging_tab_ = nullptr;
  if (press_tab_ == tab) press_ = Press::None;
  if (selected_ == tab) {
    selected_ = nullptr;
    if (tabs_.count()) selected_ = tabs_[static_cast<uint64_t>(index) < tabs_.count() ? static_cast<uint64_t>(index) : tabs_.count() - 1];
  }
  apply_selection();
  return true;
}

void TabFrame::close_tab(Frame* tab) {
  UiGuard lock(this);
  Window* w = window();
  if (!remove_tab(tab)) return;
  tab->run_closed_handler(w);
  if (tabs_.count() == 0) close();
}

void TabFrame::select_tab(Frame* tab) {
  UiGuard lock(this);
  if (index_of(tab) < 0 || selected_ == tab) return;
  selected_ = tab;
  apply_selection();
}

void TabFrame::step_selection(int32_t direction, bool wrap) {
  const int32_t n = static_cast<int32_t>(tabs_.count());
  if (n < 2) return;
  const int32_t at = index_of(selected_);
  int32_t target = (at < 0 ? 0 : at) + direction;
  if (!wrap && (target < 0 || target >= n)) return;
  select_tab(tabs_[static_cast<uint64_t>((target + n) % n)]);
}

void TabFrame::on_mouse_wheel(const MouseArgs& args) {
  if (args.wheel == 0 || !enabled() || !contains(strip_rect(), args.x, args.y)) return;
  step_selection(args.wheel < 0 ? 1 : -1, false);
}

// Shows the selected tab only, moves the focus into it, and takes over its title.
void TabFrame::apply_selection() {
  for (uint32_t i = 0; i < tabs_.count(); ++i) tabs_[i]->set_visible(tabs_[i] == selected_);
  if (selected_) set_title(selected_->title());
  if (Window* w = window()) {
    Control* f = w->focus();
    bool inside = false;
    for (Control* c = f; c; c = c->parent())
      if (c == selected_) inside = true;
    if (f && !inside && f != this) w->set_focus(nullptr);
    if (active_) w->set_focus_scope(this);
  }
  invalidate();
}

void TabFrame::tab_title_changed(Frame* tab) {
  if (tab == selected_) set_title(tab->title());
  else invalidate();
}

void TabFrame::manager_changed() {
  for (uint32_t i = 0; i < tabs_.count(); ++i) __atomic_store_n(&tabs_[i]->manager_, manager_, __ATOMIC_RELEASE);
}

// The window was closed by the manager: the tabs are closed with it.
void TabFrame::window_closed(Window* window) {
  UiGuard lock(this);
  while (tabs_.count()) {
    Frame* tab = tabs_[0];
    remove_tab(tab);
    tab->run_closed_handler(window);
  }
}

void TabFrame::layout() {
  const Rect2D area = client_rect();
  for (uint32_t i = 0; i < tabs_.count(); ++i) tabs_[i]->set_bounds(area);
  Frame::layout();
}

// --- Painting ---------------------------------------------------------------------------------------

void TabFrame::paint(Painter& painter) {
  Frame::paint(painter);
  if (fullscreen_) return;
  const Theme& t = painter.theme();
  const Rect2D strip = strip_rect();
  painter.fill(strip, t.face);
  painter.fill({strip.x, strip.y + static_cast<int32_t>(strip.height) - 1, strip.width, 1}, t.shadow);
  for (uint32_t i = 0; i < tabs_.count(); ++i) {
    const Rect2D r = tab_rect(i);
    const bool on = tabs_[i] == selected_;
    painter.fill(r, on ? t.face : kInactiveTab);
    painter.outline(r, t.shadow);
    if (on) painter.fill({r.x + 1, r.y + static_cast<int32_t>(r.height) - 1, r.width - 2, 1}, t.face);   // joins the tab to the page
    painter.push_frame({r.x + 4, r.y, r.width > static_cast<uint32_t>(kCloseSize + 14) ? r.width - kCloseSize - 14 : 0, r.height});
    painter.text(0, (static_cast<int32_t>(r.height) - Painter::kGlyphHeight) / 2, tabs_[i]->title(), t.text);
    painter.pop_frame();
    const Rect2D c = close_rect(i);
    painter.line(c.x + 2, c.y + 2, c.x + kCloseSize - 3, c.y + kCloseSize - 3, t.text);
    painter.line(c.x + kCloseSize - 3, c.y + 2, c.x + 2, c.y + kCloseSize - 3, t.text);
  }
  const Rect2D n = new_tab_button_rect();
  painter.fill(n, t.face);
  painter.bevel(n, press_ == Press::NewTab);
  painter.fill({n.x + 6, n.y + static_cast<int32_t>(n.height) / 2 - 1, static_cast<uint32_t>(kNewTabWidth - 12), 2}, t.text);
  painter.fill({n.x + kNewTabWidth / 2 - 1, n.y + 4, 2, n.height > 8 ? n.height - 8 : 0}, t.text);
}

// --- Mouse and keys ---------------------------------------------------------------------------------

void TabFrame::on_mouse_down(const MouseArgs& args) {
  if (args.button == MouseButton::Left && contains(strip_rect(), args.x, args.y)) {
    press_ = Press::None;
    press_tab_ = nullptr;
    if (enabled()) {
      if (contains(new_tab_button_rect(), args.x, args.y)) {
        press_ = Press::NewTab;
      } else {
        const int32_t i = tab_at_point(args.x, args.y);
        if (i >= 0) {
          press_tab_ = tabs_[static_cast<uint64_t>(i)];
          if (contains(close_rect(static_cast<uint32_t>(i)), args.x, args.y)) {
            press_ = Press::Close;
          } else {
            press_ = Press::Tab;
            press_x_ = args.x;
            press_y_ = args.y;
            select_tab(press_tab_);
          }
        }
      }
    }
    invalidate();
    return;
  }
  Frame::on_mouse_down(args);
}

void TabFrame::on_mouse_move(const MouseArgs& args) {
  if (press_ == Press::Tab && press_tab_ && !dragging_tab_) {
    const int32_t dx = args.x - press_x_, dy = args.y - press_y_;
    if (dx * dx + dy * dy >= 6 * 6) {
      Window* w = window();
      if (w && w->begin_drag(this, kDragPayload, press_tab_->title())) dragging_tab_ = press_tab_;
      press_ = Press::None;
    }
    return;
  }
  if (press_ != Press::None) return;
  Frame::on_mouse_move(args);
}

void TabFrame::on_mouse_up(const MouseArgs& args) {
  const Press press = press_;
  Frame* tab = press_tab_;
  press_ = Press::None;
  press_tab_ = nullptr;
  if (press == Press::None) {
    Frame::on_mouse_up(args);
    return;
  }
  invalidate();
  if (press == Press::NewTab && contains(new_tab_button_rect(), args.x, args.y)) {
    fire(new_tab_);
  } else if (press == Press::Close && tab) {
    const int32_t i = index_of(tab);
    if (i >= 0 && contains(close_rect(static_cast<uint32_t>(i)), args.x, args.y)) close_tab(tab);
  }
}

bool TabFrame::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  if (args.modifiers & hardware::modifier::kControl) {
    if (args.usage == key::kA + 19) {   // T
      fire(new_tab_);
      return true;
    }
    if (args.usage == key::kTab) {
      step_selection(args.modifiers & hardware::modifier::kShift ? -1 : 1);
      return true;
    }
  }
  return Frame::on_key(args);
}

// --- Dragging tabs between windows --------------------------------------------------------------------

bool TabFrame::accepts_drop(StringView text, int32_t, int32_t y, const Control* source) const {
  return source && text.equals(kDragPayload) && y < strip_rect().y + static_cast<int32_t>(strip_rect().height);
}

void TabFrame::on_drop(StringView, int32_t x, int32_t, const Control* source) {
  UiGuard lock(this);
  // (only tab windows start these drags)
  TabFrame* from = const_cast<TabFrame*>(static_cast<const TabFrame*>(source));
  Frame* tab = from->dragging_tab_;
  from->dragging_tab_ = nullptr;
  if (!tab || tab->host_ != from) return;
  int32_t at = static_cast<int32_t>(tabs_.count());
  for (uint32_t i = 0; i < tabs_.count(); ++i) {
    const Rect2D r = tab_rect(i);
    if (x < r.x + static_cast<int32_t>(r.width) / 2) {
      at = static_cast<int32_t>(i);
      break;
    }
  }
  if (from == this) {   // moved along the strip
    const int32_t old = index_of(tab);
    if (old < 0) return;
    if (at > old) --at;
    if (at == old) return;
    tabs_.remove_at(static_cast<uint64_t>(old));
    tabs_.insert(static_cast<uint64_t>(at), tab);
    invalidate();
    return;
  }
  from->remove_tab(tab);
  add_tab(tab, at);
  if (from->tab_count() == 0) from->close();
  activate();
}

void TabFrame::on_drag_ended(bool dropped, int32_t x, int32_t y) {
  if (dropped) return;   // (on_drop takes the tab from dragging_tab_)
  Frame* tab = dragging_tab_;
  dragging_tab_ = nullptr;
  if (!tab || x < 0 || tabs_.count() < 2) return;   // cancelled, or the only tab: nothing to take out
  torn_tab_ = tab;
  torn_x_ = x;
  torn_y_ = y;
  fire(torn_off_);
}

}  // namespace gui
