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
#include "Gui/Controls/ContextMenu.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

Window::Window(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, const Theme& theme,
               const graphics::Extent2D& size)
    : Control(allocator), renderer_(&renderer), theme_(theme), title_(allocator), drag_text_(allocator), drag_label_(allocator), drop_text_(allocator), pending_(allocator) {
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

void Window::set_renderer(graphics::IRenderer2D& renderer) {
  UiGuard lock(this);
  renderer_ = &renderer;
  const graphics::Extent2D size = renderer.extent();
  bounds_ = {0, 0, size.width, size.height};
  layout();
  mark_dirty();
}

void Window::restart_caret() {
  caret_epoch_us_ = now_us_;
  __atomic_store_n(&caret_hidden_, 0, __ATOMIC_RELEASE);
}

void Window::tick(uint64_t now_us) {
  UiGuard lock(this);
  now_us_ = now_us;
  const uint8_t hidden = ((now_us - caret_epoch_us_) / kCaretHalfPeriodUs) & 1;   // (an epoch in the future cannot happen: it is only ever set to now_us_)
  if (hidden == __atomic_load_n(&caret_hidden_, __ATOMIC_RELAXED)) return;
  __atomic_store_n(&caret_hidden_, hidden, __ATOMIC_RELEASE);
  if ((focus_ && focus_->shows_caret()) || (popup_ && popup_->shows_caret())) mark_dirty();
}

void Window::mark_dirty() {
  if (!__atomic_exchange_n(&dirty_, true, __ATOMIC_ACQ_REL)) frame_event_.set();
}

void Window::render() {
  {
    UiGuard lock(this);
    // Layout comes first and the flag is cleared after it: controls adjust themselves while laying out (scroll bars, visibility) and so ask for a
    // repaint, which this frame is about to give. Cleared before, those requests would start another frame, whose layout asks again, and a window
    // holding a grid or list would repaint non-stop. Changes made while we draw (below) still ask for the next frame.
    layout();
    __atomic_store_n(&dirty_, false, __ATOMIC_RELEASE);
    __atomic_store_n(&frame_interval_us_, uint64_t{0}, __ATOMIC_RELEASE);
    Painter painter(*renderer_, theme_);
    paint_tree(painter);
    if (popup_) popup_->paint_tree(painter);
    if (drag_source_) {
      // The dragged item follows the pointer: its name in a small box.
      painter.renderer().reset_clip();
      const StringView name = drag_label_.empty() ? drag_text_.view().substr(drag_text_.view().rfind('/') + 1) : drag_label_.view();
      const int32_t w = Painter::text_width(name) + 8, h = Painter::kGlyphHeight + 4;
      const int32_t bx = cursor_.x + 14, by = cursor_.y + 14;
      painter.fill({bx - 1, by - 1, static_cast<uint32_t>(w + 2), static_cast<uint32_t>(h + 2)}, theme_.dark);
      painter.fill({bx, by, static_cast<uint32_t>(w), static_cast<uint32_t>(h)}, theme_.selection);
      painter.text(bx + 4, by + 2, name, theme_.selection_text);
    }
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
        renderer_->fill_triangle({x - 1, y - 1}, {x + 13, y + 11}, {x + 3, y + 13}, theme_.highlight);
        renderer_->fill_triangle({x, y}, {x + 11, y + 10}, {x + 3, y + 11}, theme_.dark);
      } else if (shape == CursorShape::Crosshair) {
        for (int pass = 0; pass < 2; ++pass) {  // a white outline, then the black cross
          const int32_t reach = pass == 0 ? 8 : 7;
          const int32_t cx = cursor_.x, cy = cursor_.y;
          const uint32_t color = pass == 0 ? theme_.highlight : theme_.dark;
          const int32_t w = pass == 0 ? 3 : 1;
          painter.fill({cx - reach, cy - w / 2, static_cast<uint32_t>(2 * reach + 1), static_cast<uint32_t>(w)}, color);
          painter.fill({cx - w / 2, cy - reach, static_cast<uint32_t>(w), static_cast<uint32_t>(2 * reach + 1)}, color);
        }
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
            renderer_->fill_triangle({tx, ty}, {bx + w * px, by + w * py}, {bx - w * px, by - w * py}, color);
          }
          const float half = 1.0f + grow;
          for (float t = -4; t <= 4; t += 1) {
            const float cx = x + t * dx * step, cy = y + t * dy * step;
            renderer_->fill_triangle({cx - half * px - half, cy - half * py - half}, {cx + half * px + half, cy + half * py - half},
                                    {cx + half * px + half, cy + half * py + half}, color);
            renderer_->fill_triangle({cx - half * px - half, cy - half * py - half}, {cx + half * px + half, cy + half * py + half},
                                    {cx - half * px - half, cy - half * py + half}, color);
          }
        }
      }
    }
  }
  // Presenting needs no access to the controls: the UI may change while the frame goes out.
  renderer_->reset_clip();
  renderer_->present();
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
  if (pressed) restart_caret();
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
    if (drag_source_) finish_drag(x, y);
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
  if (button == MouseButton::Right) {  // a right click opens the context menu of the control, or the nearest one around it that has one
    for (Control* c = target; c; c = c->parent_) {
      if (!c->context_menu_) continue;
      const Point o = c->window_origin();
      if (!c->accepts_context_menu(x - o.x, y - o.y)) continue;
      c->prepare_context_menu(x - o.x, y - o.y);
      c->context_menu_->show(*this, x, y);
      return;
    }
  }
  captured_ = target;
  MouseArgs a;
  a.x = x - origin.x;
  a.y = y - origin.y;
  a.button = button;
  target->on_mouse_down(a);
}

bool Window::begin_drag(Control* source, StringView text, StringView label) {
  UiGuard lock(this);
  if (drag_source_ || !source || captured_ != source || text.empty() || !drag_text_.assign(text) || !drag_label_.assign(label)) return false;
  drag_source_ = source;
  mark_dirty();
  return true;
}

void Window::cancel_drag() {
  UiGuard lock(this);
  if (!drag_source_) return;
  Control* source = drag_source_;
  drag_source_ = nullptr;
  drag_text_.clear();
  mark_dirty();
  source->on_drag_ended(false, -1, -1);
}

void Window::finish_drag(int32_t x, int32_t y) {
  Control* source = drag_source_;
  drag_source_ = nullptr;
  Point origin;
  bool dropped = false;
  for (Control* c = target_at(x, y, &origin); c; c = c->parent_) {
    if (!c->effectively_enabled()) break;   // behind a modal window, say
    const Point o = c->window_origin();
    if (!c->accepts_drop(drag_text_.view(), x - o.x, y - o.y, source)) continue;
    if (!drop_text_.assign(drag_text_.view())) break;
    drop_target_ = c;
    drop_source_ = source;
    drop_point_ = {x - o.x, y - o.y};
    dispatch({&Window::run_drop, this}, *c);
    dropped = true;
    break;
  }
  drag_text_.clear();
  if (source) source->on_drag_ended(dropped, x, y);
}

void Window::run_drop(void* window, Control& target) {
  auto* w = static_cast<Window*>(window);
  String text(w->allocator());
  const Control* source;
  Point at;
  {
    UiGuard lock(w);
    if (w->drop_target_ != &target) return;
    w->drop_target_ = nullptr;
    text.assign(w->drop_text_.view());
    source = w->drop_source_;
    at = w->drop_point_;
  }
  target.on_drop(text.view(), at.x, at.y, source);
}

void Window::mouse_wheel(int32_t x, int32_t y, int32_t delta) {
  UiGuard lock(this);
  Point origin;
  Control* target = target_at(x, y, &origin);
  if (!target || !target->effectively_enabled()) return;
  while (target->wheel_to_parent() && target->parent()) target = target->parent();   // labels, meters and panels hand the wheel up
  const Point at = target->window_origin();
  MouseArgs a;
  a.x = x - at.x;
  a.y = y - at.y;
  a.wheel = delta;
  target->on_mouse_wheel(a);
  mark_dirty();
}

void Window::key(const KeyArgs& args) {
  UiGuard lock(this);
  restart_caret();
  mark_dirty();
  if (key_filter_ && key_filter_(key_filter_context_, args)) return;
  if (drag_source_ && args.usage == hardware::key::kEscape) {
    cancel_drag();
    return;
  }
  if (popup_) {
    popup_->on_key(args);
    return;
  }
  if (args.usage == 0x65 || (args.usage == hardware::key::kF1 + 9 && (args.modifiers & hardware::modifier::kShift))) {  // Menu key, Shift+F10
    for (Control* c = focus_; c; c = c->parent_) {
      if (!c->context_menu_ || !c->effectively_enabled()) continue;
      const Point o = c->window_origin();
      c->prepare_context_menu(-1, -1);   // no pointer: the control uses what it has selected
      c->context_menu_->show(*this, o.x + 8, o.y + static_cast<int32_t>(c->bounds().height), true);
      return;
    }
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
  if (drag_source_ && is_inside(control, drag_source_)) {
    drag_source_ = nullptr;
    drag_text_.clear();
  }
  if (drop_target_ && is_inside(control, drop_target_)) drop_target_ = nullptr;
  if (popup_owner_ && is_inside(control, popup_owner_)) close_popup();
}

}  // namespace gui

namespace gui {

// --- Handler thread, background work, destruction -----------------------------------

bool Window::start_handler_thread() {
  if (!kernel::running() || has_handler_thread()) return false;
  {
    kernel::ScopedMutex lock(queue_mutex_);
    handler_stop_ = false;
    if (!spawn_worker()) return false;
  }
  __atomic_store_n(&handlers_on_, true, __ATOMIC_RELEASE);
  return true;
}

void Window::handler_main(void* window) { static_cast<Window*>(window)->run_handlers(); }

// With queue_mutex_ held.
bool Window::spawn_worker() {
  if (worker_count_ >= kMaxHandlerThreads) return false;
  kernel::Thread* thread = kernel::spawn(&Window::handler_main, this, "gui-handlers");
  if (!thread) return false;
  workers_[worker_count_++].thread = thread;
  return true;
}

bool Window::lane_busy(const Control* lane) const {
  for (const Worker& worker : workers_)
    if (worker.busy && worker.lane == lane) return true;
  return false;
}

bool Window::any_busy() const {
  for (const Worker& worker : workers_)
    if (worker.busy) return true;
  return false;
}

const Control* Window::lane_of(const Control& sender) {
  kernel::ScopedRecursiveMutex guard(ui_mutex_);  // (the tree must not change while its parents are followed)
  for (const Control* c = &sender; c; c = c->parent())
    if (c->handler_lane()) return c;
  return nullptr;  // the shared lane: handlers of the desktop itself
}

void Window::run_handlers() {
  Worker* self = nullptr;
  for (;;) {
    queue_signal_.acquire_for(100000);  // (the timeout makes missed wake-ups harmless)
    Pending item;
    {
      kernel::ScopedMutex lock(queue_mutex_);
      if (!self) {
        for (Worker& worker : workers_)
          if (worker.thread == kernel::current_thread()) self = &worker;
        if (!self) continue;  // (the spawner has not recorded the thread yet)
      }
      uint64_t at = 0;
      while (at < pending_.count() && lane_busy(pending_[at].lane)) ++at;
      if (at == pending_.count()) {
        if (handler_stop_ && pending_.count() == 0) return;
        continue;
      }
      item = pending_[at];
      pending_.remove_at(at);
      self->lane = item.lane;
      self->sender = item.sender;
      self->busy = true;
    }
    item.handler(*item.sender);  // no lock held: this may take as long as it likes
    kernel::ScopedMutex lock(queue_mutex_);
    self->busy = false;
    self->lane = nullptr;
    self->sender = nullptr;
    if (pending_.count() > 0) queue_signal_.release();  // what waited for this lane can go on
  }
}

void Window::stop_handler_thread() {
  uint32_t count = 0;
  {
    kernel::ScopedMutex lock(queue_mutex_);
    if (!has_handler_thread()) return;
    for (const Worker& worker : workers_)
      if (worker.thread == kernel::current_thread()) return;  // (called from a handler)
    handler_stop_ = true;
    count = worker_count_;
  }
  queue_signal_.release(count);  // the threads first run what is queued, then see the flag
  for (uint32_t i = 0; i < count; ++i) kernel::join(workers_[i].thread);
  kernel::ScopedMutex lock(queue_mutex_);
  for (Worker& worker : workers_) worker = Worker();
  worker_count_ = 0;
  __atomic_store_n(&handlers_on_, false, __ATOMIC_RELEASE);
}

void Window::dispatch(const Handler& handler, Control& sender) {
  if (!handler) return;
  if (handler.inline_call || !has_handler_thread()) {
    handler(sender);
    return;
  }
  const Control* lane = lane_of(sender);
  bool queued = false;
  {
    kernel::ScopedMutex lock(queue_mutex_);
    if (!handler_stop_) queued = pending_.push_back(Pending{handler, &sender, lane});
    if (queued && !lane_busy(lane)) {  // another lane can run at once: make sure a thread is free for it
      uint32_t idle = 0;
      for (uint32_t i = 0; i < worker_count_; ++i)
        if (!workers_[i].busy) ++idle;
      if (idle < pending_.count()) spawn_worker();  // (failing is fine: the threads there will get to it)
    }
  }
  if (queued) queue_signal_.release();
  else handler(sender);  // stopping, or out of memory: run it here rather than lose it
}

bool Window::wait_for_handlers(uint64_t timeout_us) {
  for (uint64_t waited = 0;; waited += 500) {
    {
      kernel::ScopedMutex lock(queue_mutex_);
      if (pending_.count() == 0 && !any_busy()) return true;
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
    // Queued handlers of this control are dropped. (Their semaphore counts stay: a handler thread just
    // finds the queue shorter than the count and goes back to waiting.)
    kernel::ScopedMutex lock(queue_mutex_);
    for (uint64_t i = 0; i < pending_.count();) {
      if (pending_[i].sender == control) pending_.remove_at(i);
      else ++i;
    }
  }
  // A handler of this control that is running right now must finish before the control goes away - unless we
  // are that handler (a control destroying itself from its own callback).
  for (;;) {
    {
      kernel::ScopedMutex lock(queue_mutex_);
      bool running = false;
      for (const Worker& worker : workers_)
        if (worker.busy && worker.sender == control && worker.thread != kernel::current_thread()) running = true;
      if (!running) break;
    }
    kernel::sleep_us(200);
  }
  kernel::ScopedRecursiveMutex guard(ui_mutex_);
  if (focus_ == control) focus_ = nullptr;
  if (focus_scope_ == control) focus_scope_ = nullptr;
  if (hover_ == control) hover_ = nullptr;
  if (captured_ == control) captured_ = nullptr;
  if (drag_source_ == control) {
    drag_source_ = nullptr;
    drag_text_.clear();
  }
  if (drop_target_ == control || drop_source_ == control) drop_target_ = nullptr;
  if (popup_owner_ == control) popup_owner_ = nullptr;
  if (popup_ == control) popup_ = nullptr;
}

}  // namespace gui
