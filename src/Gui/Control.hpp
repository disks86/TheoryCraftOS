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

// The base of every GUI element, and the layout engine.
//
// Any control can hold children, which is how windows and panels nest. Bounds
// are relative to the parent's top left. layout() places the children of a
// control inside its client area by their DockStyle, in the order they were
// added: Top/Bottom/Left/Right children take a strip of the area that is left
// (their own height or width, the full length of the other side), a Fill child
// takes what remains, None children keep the bounds they were given. Add Fill
// children last, as later children only get what a Fill child left (nothing).
//
// Ownership: a control does not own its children or free anything; the code that
// creates controls keeps them alive for as long as they are in a tree. Controls
// use a protected, non-virtual destructor like every interface in this OS,
// so they are never deleted through Control*.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Gui/GuiTypes.hpp"
#include "Gui/Painter.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"

namespace kernel {
class Clipboard;
}

namespace gui {

class Window;
class ContextMenu;

// Threading. The control tree of a Window is protected by one recursive mutex (Window::ui_mutex()):
// painting, input delivery and every public mutator of a control take it, so any thread may call
// the setters (set_text, add, set_bounds, Frame/WindowManager operations, ...) while the window is being
// painted and fed input, with no corruption. Plain getters (text(), bounds(), ...) do not lock and
// return references into live state: from another thread read through Window::locked(). A control that
// is not in a window yet needs no lock (build the tree, then show it).
class UiGuard {
 public:
  explicit UiGuard(const Control* control);
  ~UiGuard();
  UiGuard(const UiGuard&) = delete;
  UiGuard& operator=(const UiGuard&) = delete;

 private:
  kernel::RecursiveMutex* mutex_;
};

class Control {
 public:
  Control(const Control&) = delete;
  Control& operator=(const Control&) = delete;

  // --- Geometry and layout
  const Rect2D& bounds() const { return bounds_; }
  // Absolute placement for DockStyle::None controls; docking overwrites it.
  void set_bounds(const Rect2D& bounds) { UiGuard lock(this); bounds_ = bounds; invalidate(); }
  DockStyle dock() const { return dock_; }
  void set_dock(DockStyle dock) { UiGuard lock(this); dock_ = dock; invalidate(); }
  // The size a docked control asks for (height for Top/Bottom, width for Left/Right). 0 means "use
  // default_size()".
  void set_size(int32_t width, int32_t height) { UiGuard lock(this); size_ = {width, height}; invalidate(); }
  Size preferred_size() const {
    const Size d = default_size();
    return {size_.width > 0 ? size_.width : d.width, size_.height > 0 ? size_.height : d.height};
  }
  const Insets& padding() const { return padding_; }
  void set_padding(const Insets& padding) { UiGuard lock(this); padding_ = padding; invalidate(); }
  // The area inside the padding, in this control's coordinates.
  virtual Rect2D client_rect() const;
  // Places the children (and, recursively, theirs).
  virtual void layout();

  // --- Tree
  bool add(Control* child);  // false if it is already in a tree or memory ran out
  bool remove(Control* child);
  Control* parent() const { return parent_; }
  uint32_t child_count() const { return static_cast<uint32_t>(children_.count()); }
  Control* child(uint32_t index) const { return index < children_.count() ? children_[index] : nullptr; }
  // The window this control is in (nullptr while it is not in one). Safe from any thread.
  Window* window() const { return __atomic_load_n(&attached_, __ATOMIC_ACQUIRE); }
  // The system clipboard of the window this control is in (nullptr while it is in none, or the window has none).
  kernel::Clipboard* clipboard() const;
  // Position of this control's top left in window coordinates.
  Point window_origin() const;

  // --- State
  bool visible() const { return visible_; }
  void set_visible(bool v) { UiGuard lock(this); visible_ = v; invalidate(); }
  bool enabled() const { return enabled_; }
  void set_enabled(bool v) { UiGuard lock(this); enabled_ = v; invalidate(); }
  // False if this control or any ancestor is disabled (a disabled window disables everything in it).
  bool effectively_enabled() const {
    for (const Control* c = this; c; c = c->parent_)
      if (!c->enabled_) return false;
    return true;
  }
  bool hovered() const { return hovered_; }
  bool focused() const { return focused_; }
  virtual bool focusable() const { return false; }
  // Asks for a repaint.
  void invalidate() const;

  // --- Painting. paint_tree() paints this control and then its children, clipped to the bounds.
  void paint_tree(Painter& painter);
  virtual void paint(Painter&) {}

  // --- Input. Coordinates are relative to this control.
  virtual void on_mouse_enter() {}
  virtual void on_mouse_leave() {}
  virtual void on_mouse_move(const MouseArgs&) {}
  virtual void on_mouse_down(const MouseArgs&) {}
  virtual void on_mouse_up(const MouseArgs&) {}
  virtual void on_mouse_wheel(const MouseArgs&) {}
  // True for a control that does nothing with the wheel itself (a label, a meter, a plain panel): the wheel then goes to its parent, so a scrolling container
  // scrolls wherever the pointer is over it.
  virtual bool wheel_to_parent() const { return false; }
  // Return true if the key was used; otherwise it goes to the parent.
  virtual bool on_key(const KeyArgs&) { return false; }
  virtual void on_focus_changed(bool) {}
  // Called on every ancestor when the mouse goes down on this control or one of its descendants
  // (before the control itself gets the press): a window manager raises the window that was clicked.
  virtual void on_descendant_pressed(Control* /*child_of_this*/) {}
  // Called on the owner of a popup (Window::open_popup) when the popup closes.
  virtual void on_popup_closed(Control*) {}

  // The pointer to draw while the mouse is at (x, y) of this control (or is captured by it).
  virtual CursorShape cursor_shape(int32_t /*x*/, int32_t /*y*/) const { return CursorShape::Arrow; }

  // --- Context menu. A control with one has it opened (at the pointer) by a right-button press on it or on a control inside it that has
  // none of its own, and from the keyboard by the Menu key or Shift+F10 while it or something inside it has the focus. See ContextMenu.
  void set_context_menu(ContextMenu* menu) { UiGuard lock(this); context_menu_ = menu; }
  ContextMenu* context_menu() const { return context_menu_; }
  // Whether a right click at (x, y) of this control opens the menu (a control can keep the right button for itself in places).
  virtual bool accepts_context_menu(int32_t /*x*/, int32_t /*y*/) const { return true; }
  // Called just before the menu opens for a click at (x, y) of this control: a list selects the item under the pointer.
  virtual void prepare_context_menu(int32_t /*x*/, int32_t /*y*/) {}

  // --- Drag and drop (see Window::begin_drag). The window offers a dropped item - `text`, e.g. the path of a file - to the control under the
  // pointer and then to each control around it, until one accepts. (x, y) is the pointer in this control's coordinates, `source` the control
  // the drag started in (it may be this one). on_drop() runs on the handler thread (see Handler), like a fired handler.
  virtual bool accepts_drop(StringView /*text*/, int32_t /*x*/, int32_t /*y*/, const Control* /*source*/) const { return false; }
  virtual void on_drop(StringView /*text*/, int32_t /*x*/, int32_t /*y*/, const Control* /*source*/) {}
  // Called on the control a drag started in when the button was released (dropped: some control took it) or Escape cancelled it (x and y are -1).
  // (x, y) is the pointer in window coordinates. Runs on the input thread under the UI lock.
  virtual void on_drag_ended(bool /*dropped*/, int32_t /*x*/, int32_t /*y*/) {}

  // The deepest visible control under (x, y) (this control's coordinates), or nullptr.
  Control* hit_test(int32_t x, int32_t y);

 protected:
  explicit Control(allocators::IAllocator& allocator) : allocator_(allocator), children_(allocator) {}
  // Tells the window (if any) that this control is gone, so nothing queued for it runs afterwards.
  ~Control();

  // Runs a handler the way the window wants it run (see Handler): on its handler thread if it has one.
  void fire(const Handler& handler);

  virtual Size default_size() const { return {0, 0}; }
  // Moves `child` to the end of the child list: painted last (on top) and hit first.
  bool move_child_to_front(Control* child);
  allocators::IAllocator& allocator() const { return allocator_; }

  // Places `child` the way layout() does: no repaint request. (Window::render() lays out every frame; a layout that called set_bounds() would ask for
  // the next frame each time and the window would never rest.)
  static void place(Control& child, const Rect2D& bounds) { child.bounds_ = bounds; }

  Rect2D bounds_;

 private:
  friend class Window;

  allocators::IAllocator& allocator_;
  List<Control*> children_;
  Control* parent_ = nullptr;
  ContextMenu* context_menu_ = nullptr;
  Window* attached_ = nullptr;  // the window whose tree this control is in (also set on a window's popup)
  Window* history_window_ = nullptr;  // the last window it was in: told when the control is destroyed (so the window must outlive its controls)
  void attach(Window* window);   // this control and everything below it
  DockStyle dock_ = DockStyle::None;
  Size size_;
  Insets padding_;
  bool visible_ = true, enabled_ = true, hovered_ = false, focused_ = false;
};

}  // namespace gui
