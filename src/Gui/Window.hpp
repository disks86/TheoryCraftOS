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

// The root of a control tree: sized to the screen (or part of it), draws an
// optional title bar, turns raw input (mouse position/buttons, key presses)
// into control events and owns focus, mouse capture and the single popup
// (drop-down list or menu) that is drawn above everything else.
//
// Typical use: build controls, add them to the window, then feed input (InputPump does that from the
// keyboard and mouse drivers) from one thread while a RenderLoop thread calls render() whenever
// dirty() says something changed, and let the handler thread run what controls fire.
//
// Threading: see UiGuard (Control.hpp) for the locking rules. A window must outlive the controls in it
// (declare it first), and render() / the RenderLoop are the only users of the renderer.
#pragma once
#include "Gui/Control.hpp"
#include "Kernel/Clipboard.hpp"
#include "Utility/String.hpp"

namespace gui {

class Window final : public Control {
 public:
  Window(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, const Theme& theme,
         const graphics::Extent2D& size);
  // Waits for background work, stops the handler thread. Do not destroy a window from inside a handler.
  ~Window();

  // The title bar is drawn when the title is not empty.
  void set_title(StringView title);
  static constexpr int32_t kTitleHeight = 20;

  Rect2D client_rect() const override;
  void paint(Painter& painter) override;

  // Lays out, draws everything (children, popup, cursor) and presents. Clears dirty(). The tree is locked
  // only while it is drawn, not while the finished frame is presented. Call from one thread at a time.
  void render();
  bool dirty() const { return __atomic_load_n(&dirty_, __ATOMIC_ACQUIRE); }
  // Something changed: wakes whoever waits in wait_for_frame(). Safe from any thread.
  void mark_dirty();
  // Waits until the window was marked dirty (or the timeout passed); true if it was. For render loops.
  bool wait_for_frame(uint64_t timeout_us) { return frame_event_.wait(timeout_us); }

  // --- Threads
  kernel::RecursiveMutex& ui_mutex() { return ui_mutex_; }
  // Runs `function` (any callable) with the UI lock held: the way another thread reads state that has no
  // thread-safe accessor, e.g. window.locked([&]{ copy(label.text()); }).
  template <class F>
  void locked(F&& function) {
    kernel::ScopedRecursiveMutex guard(ui_mutex_);
    function();
  }
  // Starts the thread that runs fired handlers (see Handler); false if threads are not available (the
  // kernel has not started) or it already runs - handlers then run in the thread that fires them.
  bool start_handler_thread();
  // Runs everything still queued, then ends the thread. Must not be called with the UI lock held or from
  // a handler.
  void stop_handler_thread();
  bool has_handler_thread() const { return __atomic_load_n(&handler_thread_, __ATOMIC_ACQUIRE) != nullptr; }
  // Queues the handler (or calls it, see Handler). Safe from any thread.
  void dispatch(const Handler& handler, Control& sender);
  // Waits until no handler is queued or running; false on timeout. For shutdown and tests.
  bool wait_for_handlers(uint64_t timeout_us);
  // Runs `work` on a new thread; when it returns `done` is fired with this window as sender. The UI stays
  // live meanwhile and `work` may use any setter. With no threads it simply runs both in the caller.
  bool run_async(void (*work)(void* context), void* context, Handler done = {});
  // Waits for run_async() work to finish; false on timeout.
  bool wait_for_background(uint64_t timeout_us);
  // Draws a small arrow at the mouse position (for relative-mouse setups with no hardware cursor).
  void show_cursor(bool show) {
    UiGuard lock(this);
    cursor_visible_ = show;
    mark_dirty();
  }

  // The clipboard the controls in this window copy to and paste from (nullptr: none; controls with a private fallback
  // then use that). It must outlive the window.
  void set_clipboard(kernel::Clipboard* clipboard) { __atomic_store_n(&clipboard_, clipboard, __ATOMIC_RELEASE); }
  kernel::Clipboard* clipboard() const { return __atomic_load_n(&clipboard_, __ATOMIC_ACQUIRE); }

  // --- Input, in window coordinates.
  void mouse_move(int32_t x, int32_t y);
  void mouse_button(int32_t x, int32_t y, MouseButton button, bool pressed);
  void mouse_wheel(int32_t x, int32_t y, int32_t delta);
  void key(const KeyArgs& args);  // a key press (or repeat)
  // A function that sees every key press before anything else does (a popup, the focused control); true means it was used up. For shortcuts
  // of the whole desktop. It runs on the thread that delivers input, with the UI lock held.
  using KeyFilter = bool (*)(void* context, const KeyArgs& args);
  void set_key_filter(KeyFilter filter, void* context) {
    UiGuard lock(this);
    key_filter_ = filter;
    key_filter_context_ = context;
  }

  // --- Drag and drop between controls (also in different windows of the manager). A control that sees the mouse move far enough
  // with the button held calls begin_drag() with what it carries (`text`, e.g. a file's full path); the pointer then shows the last
  // part of the text. When the button is released the controls under the pointer are asked accept_drop() (the nearest first) and the
  // first that says yes gets on_drop(). Escape cancels. Only one drag at a time; false if one is running or `source` is not the
  // control that holds the mouse.
  // `label` is what the pointer shows (default: the last part of `text`, after the final '/').
  bool begin_drag(Control* source, StringView text, StringView label = StringView());
  bool dragging() const { return drag_source_ != nullptr; }
  StringView drag_text() const { return drag_text_.view(); }
  void cancel_drag();   // (the source is told, see Control::on_drag_ended)

  // --- Focus. Tab / Shift+Tab move it through focusable controls in tree order.
  Control* focus() const { return focus_; }
  void set_focus(Control* control);
  void focus_next(bool forward);
  // Restricts Tab/Shift+Tab (and the initial focus) to the controls inside `scope`, e.g. the active
  // window; nullptr = the whole tree. If the focus is outside the new scope it moves to the first
  // focusable control inside it (or to none).
  void set_focus_scope(Control* scope);

  // --- One popup at a time, drawn above the tree and given input first. `owner` is told when it
  // closes. (x, y) is where its top left should go; it is moved to stay inside the window. The popup
  // is not part of the tree and is not owned by the window.
  bool open_popup(Control* popup, Control* owner, int32_t x, int32_t y);
  void close_popup();
  Control* popup() const { return popup_; }
  Control* popup_owner() const { return popup_owner_; }

  // Called by Control::remove so no pointer to a removed control lingers.
  void control_removed(Control* control);
  // Called when a control in this window is destroyed: queued handlers of it are dropped (one already
  // running is waited for, except from the handler thread itself) and no pointer to it lingers.
  void control_destroyed(Control* control);

 private:
  Control* target_at(int32_t x, int32_t y, Point* origin);
  void deliver_move(Control* target, const Point& origin, int32_t x, int32_t y);
  static bool is_inside(const Control* ancestor, const Control* c);
  void finish_drag(int32_t x, int32_t y);
  static void run_drop(void* window, Control& target);
  uint32_t collect_focusable(Control* from, Control** out, uint32_t count, uint32_t capacity);

  struct Pending {
    Handler handler;
    Control* sender;
  };
  static void handler_main(void* window);
  void run_handlers();

  graphics::IRenderer2D& renderer_;
  const Theme& theme_;
  String title_;
  Control* focus_ = nullptr;
  Control* focus_scope_ = nullptr;
  Control* hover_ = nullptr;
  Control* captured_ = nullptr;
  Control* popup_ = nullptr;
  Control* popup_owner_ = nullptr;
  KeyFilter key_filter_ = nullptr;
  void* key_filter_context_ = nullptr;
  Control* drag_source_ = nullptr;
  String drag_text_, drag_label_;
  Control* drop_target_ = nullptr;   // the drop waiting for the handler thread
  const Control* drop_source_ = nullptr;
  String drop_text_;
  Point drop_point_;
  kernel::Clipboard* clipboard_ = nullptr;
  Point cursor_;
  bool cursor_visible_ = false;
  bool dirty_ = true;

  kernel::RecursiveMutex ui_mutex_;
  kernel::Event frame_event_{true};  // auto-reset: set when the window becomes dirty
  // The handler queue.
  kernel::Mutex queue_mutex_;
  kernel::Semaphore queue_signal_{0};
  List<Pending> pending_;
  kernel::Thread* handler_thread_ = nullptr;
  const Control* running_sender_ = nullptr;
  bool running_handler_ = false;
  bool handler_stop_ = false;
  uint32_t background_ = 0;  // run_async() jobs in flight
};

}  // namespace gui
