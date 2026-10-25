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

// The toasts of the desktop: small notices that appear at the bottom right corner, stack upwards (the newest at the bottom), and go away by
// themselves after a few seconds or when clicked. They are how a program tells the user something without taking the focus or stopping
// them (a modal NoticeDialog does that): "copy finished", "printer out of paper", "download complete".
// The center is one control that sits over the other controls of its parent (add it after them); it shrinks to the toasts it shows, so everything
// else stays reachable, and it paints nothing while it has none. post() may be called from any thread. The time comes from tick(), which the
// desktop calls with the uptime: a toast's time starts at the first tick after it was posted. Clicking a toast fires its handler (if it
// has one) and removes it; clicking its [x] only removes it. When more than kMaxToasts are showing, the oldest goes.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/NoticeDialog.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace gui {

class ToastCenter : public Control {
 public:
  static constexpr uint32_t kMaxToasts = 5, kMaxLines = 3, kTitleChars = 36, kDefaultDurationMs = 5000;
  static constexpr int32_t kWidth = 340, kMargin = 8, kGap = 6, kPad = 6, kStripe = 4;

  explicit ToastCenter(allocators::IAllocator& allocator) : Control(allocator), toasts_(allocator) {}

  // Shows a toast: a title (one line, cut at kTitleChars) and a message (word-wrapped over up to kMaxLines lines, longer text ends in "...").
  // It stays `duration_ms` (0: until clicked). `on_click` runs when the toast is clicked, with the center as its sender. Returns the toast's
  // number (never 0), or 0 if memory ran out or both texts are empty.
  uint32_t post(StringView title, StringView message, NoticeKind kind = NoticeKind::Information, uint32_t duration_ms = kDefaultDurationMs,
                Handler on_click = Handler());
  // Removes a toast (false: there is none with that number - it timed out or was clicked already).
  bool dismiss(uint32_t id);
  void clear();
  // Removes the toasts whose time is up; `uptime_us` is the current uptime. Returns how many went.
  uint32_t tick(uint64_t uptime_us);

  uint32_t count() const { return static_cast<uint32_t>(toasts_.count()); }
  bool showing(uint32_t id) const;
  // The texts of the toast with that number, "" if it is gone. (`message` has the lines of the wrapped text joined by '\n'.)
  StringView title_of(uint32_t id) const;
  StringView message_of(uint32_t id) const;
  NoticeKind kind_of(uint32_t id) const;
  // The rectangle toasts are stacked in the corner of, in the parent's coordinates (default: the whole parent). The desktop gives the work area of
  // the primary monitor.
  void set_area(const Rect2D& area) { UiGuard lock(this); area_ = area; has_area_ = true; invalidate(); }
  void clear_area() { UiGuard lock(this); has_area_ = false; invalidate(); }
  // The bounds of toast number `index` (0 is the oldest, at the top) in the parent's coordinates; an empty rectangle if there is none.
  Rect2D toast_bounds(uint32_t index) const;

  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;

  static constexpr uint32_t kInfoColor = 0xFF2060C0, kWarningColor = 0xFFE0A000, kErrorColor = 0xFFC02020, kBackground = 0xFFF8F8F8;

 private:
  struct Toast {
    explicit Toast(allocators::IAllocator& allocator) : title(allocator), body(allocator) {}
    uint32_t id = 0;
    NoticeKind kind = NoticeKind::Information;
    String title, body;     // body: the wrapped lines joined by '\n'
    uint32_t lines = 0;
    uint64_t duration_us = 0, deadline_us = 0;   // deadline 0: the clock has not been seen yet
    Handler on_click;
  };

  static int32_t height_of(const Toast& toast);
  static uint32_t color_of(NoticeKind kind);
  int32_t stack_height() const;
  // The toast whose rectangle holds y (this control's coordinates), or count() if none.
  uint32_t toast_at(int32_t y) const;

  List<Toast> toasts_;
  Rect2D area_;
  bool has_area_ = false;
  uint32_t next_id_ = 1;
};

}  // namespace gui
