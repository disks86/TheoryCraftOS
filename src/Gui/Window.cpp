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

#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

Window::Window(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, const Theme& theme,
               const graphics::Extent2D& size)
    : Control(allocator), renderer_(renderer), theme_(theme), title_(allocator), pending_(allocator) {
  attach(this);  // (no children yet)
  bounds_ = {0, 0, size.width, size.height};
}

Window::~Window() {
  wait_for_background(30000000);
  stop_handler_thread();
  // Controls outliving the window must not call back into it (they should not outlive it at all).
  __atomic_store_n(&attached_, static_cast<Window*>(nullptr), __ATOMIC_RELEASE);
  history_window_ = nullptr;  // (~Control must not call back into a window that is being destroyed)
}

void Window::set_title(StringView title) {
  UiGuard lock(this);
  title_.assign(title);
  mark_dirty();
}

Rect2D Window::client_rect() const {
  Rect2D r = Control::client_rect();
  if (!title_.empty()) {
    const uint32_t bar = kTitleHeight;
    r.y += kTitleHeight;
    r.height = r.height > bar ? r.height - bar : 0;
  }
  return r;
}

void Window::paint(Painter& painter) {
  painter.fill({0, 0, bounds_.width, bounds_.height}, theme_.face);
  if (!title_.empty()) {
    painter.fill({0, 0, bounds_.width, static_cast<uint32_t>(kTitleHeight)}, theme_.title);
    painter.text(6, (kTitleHeight - Painter::kGlyphHeight) / 2, title_.view(), theme_.title_text);
  }
}

void Window::mark_dirty() {
  if (!__atomic_exchange_n(&dirty_, true, __ATOMIC_ACQ_REL)) frame_event_.set();
}

void Window::render() {
  {
    UiGuard lock(this);
    __atomic_store_n(&dirty_, false, __ATOMIC_RELEASE);  // changes made while we draw ask for the next frame
    layout();
    Painter painter(renderer_, theme_);
    paint_tree(painter);
    if (popup_) popup_->paint_tree(painter);
    if (cursor_visible_) {
      painter.renderer().reset_clip();
      const float x = static_cast<float>(cursor_.x), y = static_cast<float>(cursor_.y);
      Control* under = captured_;
      Point origin;
      if (under) origin = under->window_origin();
      else under = target_at(cursor_.x, cursor_.y, &origin);
      const CursorShape shape = under ? under->cursor_shape(cursor_.x - origin.x, cursor_.y - origin.y) : CursorShape::Arrow;
      if (shape == CursorShape::Arrow) {
        // A classic arrow: black triangle with a white edge.
        renderer_.fill_triangle({x - 1, y - 1}, {x + 13, y + 11}, {x + 3, y + 13}, theme_.highlight);
        renderer_.fill_triangle({x, y}, {x + 11, y + 10}, {x + 3, y + 11}, theme_.dark);
      } else {
        // A double arrow along the resize direction, centered on the pointer.
        const int32_t dx = shape == CursorShape::ResizeVertical ? 0 : 1;
        const int32_t dy = shape == CursorShape::ResizeHorizontal ? 0 : shape == CursorShape::ResizeDiagonalUp ? -1 : 1;
        const int32_t px = -dy, py = dx;  // perpendicular
        const float step = dx && dy ? 0.75f : 1.0f;
        for (int pass = 0; pass < 2; ++pass) {  // white outline, then the black arrow on top
          const float grow = pass == 0 ? 1.5f : 0.0f;
          const uint32_t color = pass == 0 ? theme_.highlight : theme_.dark;
          for (int sign = -1; sign <= 1; sign += 2) {
            const float s = static_cast<float>(sign) * step;
            const float tx = x + s * (9 + grow) * dx, ty = y + s * (9 + grow) * dy;
            const float bx = x + s * 3 * dx, by = y + s * 3 * dy;
            const float w = 4.5f + grow;
            renderer_.fill_triangle({tx, ty}, {bx + w * px, by + w * py}, {bx - w * px, by - w * py}, color);
          }
          const float half = 1.0f + grow;
          for (float t = -4; t <= 4; t += 1) {
            const float cx = x + t * dx * step, cy = y + t * dy * step;
            renderer_.fill_triangle({cx - half * px - half, cy - half * py - half}, {cx + half * px + half, cy + half * py - half},
                                    {cx + half * px + half, cy + half * py + half}, color);
            renderer_.fill_triangle({cx - half * px - half, cy - half * py - half}, {cx + half * px + half, cy + half * py + half},
                                    {cx - half * px - half, cy - half * py + half}, color);
          }
        }
      }
    }
  }
  // Presenting needs no access to the controls: the UI may change while the frame goes out.
  renderer_.reset_clip();
  renderer_.present();
}

// --- Input -------------------------------------------------------------------

bool Window::is_inside(const Control* ancestor, const Control* c) {
  for (; c; c = c->parent_)
    if (c == ancestor) return true;
  return false;
}

Control* Window::target_at(int32_t x, int32_t y, Point* origin) {
  if (popup_ && contains(popup_->bounds(), x, y)) {
    Control* hit = popup_->hit_test(x - popup_->bounds().x, y - popup_->bounds().y);
    *origin = hit ? hit->window_origin() : Point{};
    if (hit) {  // popup children (if any) are positioned relative to the popup
      origin->x += popup_->bounds().x;
      origin->y += popup_->bounds().y;
    }
    return hit;
  }
  Control* hit = hit_test(x, y);
  *origin = hit ? hit->window_origin() : Point{};
  return hit;
}

void Window::deliver_move(Control* target, const Point& origin, int32_t x, int32_t y) {
  if (target != hover_) {
    if (hover_) {
      hover_->hovered_ = false;
      hover_->on_mouse_leave();
    }
    hover_ = target;
    if (hover_) {
      hover_->hovered_ = true;
      hover_->on_mouse_enter();
    }
  }
  if (target && target->effectively_enabled()) {
    MouseArgs a;
    a.x = x - origin.x;
    a.y = y - origin.y;
    target->on_mouse_move(a);
  }
  mark_dirty();
}

void Window::mouse_move(int32_t x, int32_t y) {
  UiGuard lock(this);
  cursor_ = {x, y};
  Point origin;
  if (captured_) {
    origin = captured_->window_origin();
    if (is_inside(popup_, captured_) && popup_) {
      origin = {popup_->bounds().x, popup_->bounds().y};
    }
    MouseArgs a;
    a.x = x - origin.x;
    a.y = y - origin.y;
    captured_->on_mouse_move(a);
    mark_dirty();
    return;
  }
  Control* target = target_at(x, y, &origin);
  deliver_move(target, origin, x, y);
}

void Window::mouse_button(int32_t x, int32_t y, MouseButton button, bool pressed) {
  UiGuard lock(this);
  cursor_ = {x, y};
  mark_dirty();
  Point origin;
  if (!pressed) {
    Control* target = captured_ ? captured_ : target_at(x, y, &origin);
    if (captured_) {
      origin = (popup_ && captured_ == popup_) ? Point{popup_->bounds().x, popup_->bounds().y} : captured_->window_origin();
    }
    captured_ = nullptr;
    if (target && target->effectively_enabled()) {
      MouseArgs a;
      a.x = x - origin.x;
      a.y = y - origin.y;
      a.button = button;
      target->on_mouse_up(a);
    }
    return;
  }
  Control* target = target_at(x, y, &origin);
  if (popup_ && !contains(popup_->bounds(), x, y)) {
    // A click outside the popup dismisses it. A click on whatever opened it only dismisses (so a
    // second click on a menu title or drop-down closes it); anything else also gets the click.
    Control* owner = popup_owner_;
    close_popup();
    if (target && owner && is_inside(owner, target)) return;
    target = target_at(x, y, &origin);
  }
  if (!target) return;
  // Ancestors hear about the press first (a window manager raises the clicked window), even when
  // the control itself is disabled - the manager decides.
  for (Control* c = target; c && c->parent_ && c != popup_; c = c->parent_) c->parent_->on_descendant_pressed(c);
  if (!target->effectively_enabled()) return;
  if (target->focusable()) set_focus(target);
  captured_ = target;
  MouseArgs a;
  a.x = x - origin.x;
  a.y = y - origin.y;
  a.button = button;
  target->on_mouse_down(a);
}

void Window::mouse_wheel(int32_t x, int32_t y, int32_t delta) {
  UiGuard lock(this);
  Point origin;
  Control* target = target_at(x, y, &origin);
  if (!target || !target->effectively_enabled()) return;
  MouseArgs a;
  a.x = x - origin.x;
  a.y = y - origin.y;
  a.wheel = delta;
  target->on_mouse_wheel(a);
  mark_dirty();
}

void Window::key(const KeyArgs& args) {
  UiGuard lock(this);
  mark_dirty();
  if (popup_) {
    popup_->on_key(args);
    return;
  }
  for (Control* c = focus_; c; c = c->parent_)
    if (c->effectively_enabled() && c->on_key(args)) return;
  if (args.usage == hardware::key::kTab) focus_next(!(args.modifiers & hardware::modifier::kShift));
}

// --- Focus -------------------------------------------------------------------

void Window::set_focus(Control* control) {
  UiGuard lock(this);
  if (control == focus_) return;
  Control* old = focus_;
  focus_ = control;
  if (old) {
    old->focused_ = false;
    old->on_focus_changed(false);
  }
  if (control) {
    control->focused_ = true;
    control->on_focus_changed(true);
  }
  mark_dirty();
}

uint32_t Window::collect_focusable(Control* from, Control** out, uint32_t count, uint32_t capacity) {
  if (!from->visible() || !from->enabled()) return count;
  if (from->focusable() && count < capacity) out[count++] = from;
  for (uint32_t i = 0; i < from->child_count(); ++i) count = collect_focusable(from->child(i), out, count, capacity);
  return count;
}

void Window::set_focus_scope(Control* scope) {
  UiGuard lock(this);
  focus_scope_ = scope;
  if (!scope || (focus_ && is_inside(scope, focus_))) return;
  Control* order[64];
  const uint32_t n = collect_focusable(scope, order, 0, 64);
  set_focus(n ? order[0] : nullptr);
}

void Window::focus_next(bool forward) {
  UiGuard lock(this);
  Control* order[64];
  const uint32_t n = collect_focusable(focus_scope_ ? focus_scope_ : this, order, 0, 64);
  if (n == 0) return;
  uint32_t at = n;  // "none" until found
  for (uint32_t i = 0; i < n; ++i)
    if (order[i] == focus_) at = i;
  const uint32_t next = at == n ? (forward ? 0 : n - 1) : (forward ? (at + 1) % n : (at + n - 1) % n);
  set_focus(order[next]);
}

// --- Popups ------------------------------------------------------------------

bool Window::open_popup(Control* popup, Control* owner, int32_t x, int32_t y) {
  UiGuard lock(this);
  if (!popup || popup->parent_) return false;
  close_popup();
  popup->attach(this);
  const Size size = popup->preferred_size();
  if (x + size.width > static_cast<int32_t>(bounds_.width)) x = static_cast<int32_t>(bounds_.width) - size.width;
  if (y + size.height > static_cast<int32_t>(bounds_.height)) y = static_cast<int32_t>(bounds_.height) - size.height;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  popup->bounds_ = {x, y, static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height)};
  popup->layout();
  popup_ = popup;
  popup_owner_ = owner;
  mark_dirty();
  return true;
}

void Window::close_popup() {
  UiGuard lock(this);
  if (!popup_) return;
  Control* popup = popup_;
  Control* owner = popup_owner_;
  popup_ = popup_owner_ = nullptr;
  if (captured_ == popup) captured_ = nullptr;
  if (hover_ == popup) hover_ = nullptr;
  popup->attach(nullptr);
  if (owner) owner->on_popup_closed(popup);
  mark_dirty();
}

void Window::control_removed(Control* control) {
  // (the caller, Control::remove, holds the UI lock)
  if (focus_scope_ && is_inside(control, focus_scope_)) focus_scope_ = nullptr;
  if (focus_ && is_inside(control, focus_)) set_focus(nullptr);
  if (hover_ && is_inside(control, hover_)) hover_ = nullptr;
  if (captured_ && is_inside(control, captured_)) captured_ = nullptr;
  if (popup_owner_ && is_inside(control, popup_owner_)) close_popup();
}

}  // namespace gui

namespace gui {

// --- Handler thread, background work, destruction -----------------------------------

bool Window::start_handler_thread() {
  if (!kernel::running() || has_handler_thread()) return false;
  handler_stop_ = false;
  kernel::Thread* thread = kernel::spawn(&Window::handler_main, this, "gui-handlers");
  if (!thread) return false;
  __atomic_store_n(&handler_thread_, thread, __ATOMIC_RELEASE);
  return true;
}

void Window::handler_main(void* window) { static_cast<Window*>(window)->run_handlers(); }

void Window::run_handlers() {
  for (;;) {
    queue_signal_.acquire();
    Pending item;
    {
      kernel::ScopedMutex lock(queue_mutex_);
      if (pending_.count() == 0) {
        if (handler_stop_) return;
        continue;
      }
      item = pending_[0];
      pending_.remove_at(0);
      running_sender_ = item.sender;
      running_handler_ = true;
    }
    item.handler(*item.sender);  // no lock held: this may take as long as it likes
    kernel::ScopedMutex lock(queue_mutex_);
    running_handler_ = false;
    running_sender_ = nullptr;
  }
}

void Window::stop_handler_thread() {
  kernel::Thread* thread = __atomic_load_n(&handler_thread_, __ATOMIC_ACQUIRE);
  if (!thread || thread == kernel::current_thread()) return;
  {
    kernel::ScopedMutex lock(queue_mutex_);
    handler_stop_ = true;
  }
  queue_signal_.release();  // the thread first runs what is queued, then sees the flag
  kernel::join(thread);
  __atomic_store_n(&handler_thread_, static_cast<kernel::Thread*>(nullptr), __ATOMIC_RELEASE);
}

void Window::dispatch(const Handler& handler, Control& sender) {
  if (!handler) return;
  if (handler.inline_call || !has_handler_thread()) {
    handler(sender);
    return;
  }
  bool queued = false;
  {
    kernel::ScopedMutex lock(queue_mutex_);
    if (!handler_stop_) queued = pending_.push_back(Pending{handler, &sender});
  }
  if (queued) queue_signal_.release();
  else handler(sender);  // stopping, or out of memory: run it here rather than lose it
}

bool Window::wait_for_handlers(uint64_t timeout_us) {
  for (uint64_t waited = 0;; waited += 500) {
    {
      kernel::ScopedMutex lock(queue_mutex_);
      if (pending_.count() == 0 && !running_handler_) return true;
    }
    if (waited >= timeout_us) return false;
    kernel::sleep_us(500);
  }
}

namespace {
struct AsyncJob {
  Window* window;
  allocators::IAllocator* allocator;
  void (*work)(void*);
  void* context;
  Handler done;
  uint32_t* counter;
};

void async_main(void* argument) {
  AsyncJob job = *static_cast<AsyncJob*>(argument);
  job.allocator->deallocate(argument);
  job.work(job.context);
  job.window->dispatch(job.done, *job.window);
  __atomic_fetch_sub(job.counter, 1, __ATOMIC_ACQ_REL);
}
}  // namespace

bool Window::run_async(void (*work)(void*), void* context, Handler done) {
  if (!work) return false;
  if (kernel::running()) {
    void* memory = allocator().allocate(sizeof(AsyncJob));
    if (memory) {
      *static_cast<AsyncJob*>(memory) = AsyncJob{this, &allocator(), work, context, done, &background_};
      __atomic_fetch_add(&background_, 1, __ATOMIC_ACQ_REL);
      if (kernel::Thread* thread = kernel::spawn(async_main, memory, "gui-async")) {
        kernel::detach(thread);
        return true;
      }
      __atomic_fetch_sub(&background_, 1, __ATOMIC_ACQ_REL);
      allocator().deallocate(memory);
    }
  }
  work(context);  // no thread to be had: do it here
  dispatch(done, *this);
  return true;
}

bool Window::wait_for_background(uint64_t timeout_us) {
  for (uint64_t waited = 0;; waited += 500) {
    if (__atomic_load_n(&background_, __ATOMIC_ACQUIRE) == 0) return true;
    if (waited >= timeout_us) return false;
    kernel::sleep_us(500);
  }
}

void Window::control_destroyed(Control* control) {
  {
    // Queued handlers of this control are dropped. (Their semaphore counts stay: the handler thread just
    // finds the queue shorter than the count and goes back to waiting.)
    kernel::ScopedMutex lock(queue_mutex_);
    for (uint64_t i = 0; i < pending_.count();) {
      if (pending_[i].sender == control) pending_.remove_at(i);
      else ++i;
    }
  }
  // A handler of this control that is running right now must finish before the control goes away - unless we
  // are that handler (a control destroying itself from its own callback).
  if (kernel::current_thread() != __atomic_load_n(&handler_thread_, __ATOMIC_ACQUIRE)) {
    for (;;) {
      {
        kernel::ScopedMutex lock(queue_mutex_);
        if (!(running_handler_ && running_sender_ == control)) break;
      }
      kernel::sleep_us(200);
    }
  }
  kernel::ScopedRecursiveMutex guard(ui_mutex_);
  if (focus_ == control) focus_ = nullptr;
  if (focus_scope_ == control) focus_scope_ = nullptr;
  if (hover_ == control) hover_ = nullptr;
  if (captured_ == control) captured_ = nullptr;
  if (popup_owner_ == control) popup_owner_ = nullptr;
  if (popup_ == control) popup_ = nullptr;
}

}  // namespace gui
