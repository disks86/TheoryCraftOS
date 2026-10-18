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

// A modal notice with one OK button: information, a warning or an error, shown with a matching title and
// icon. The message is word-wrapped over up to four lines (longer text ends in "..."). Enter, Escape, OK and
// the close button all dismiss it. One dialog is shared by the programs; showing another notice while it
// is open replaces the text. Minimize and maximize are disabled.
#pragma once
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"

namespace gui {

enum class NoticeKind : uint8_t { Information, Warning, Error };

// The coloured badge with the kind's symbol that sits left of the message.
class NoticeIcon : public Control {
 public:
  explicit NoticeIcon(allocators::IAllocator& allocator) : Control(allocator) {}
  void set_kind(NoticeKind kind) { UiGuard lock(this); kind_ = kind; invalidate(); }
  NoticeKind kind() const { return kind_; }
  void paint(Painter& painter) override;

 private:
  NoticeKind kind_ = NoticeKind::Information;
};

class NoticeDialog : public Frame {
 public:
  static constexpr int32_t kWidth = 420, kMaxLines = 4, kLineChars = 40;

  explicit NoticeDialog(allocators::IAllocator& allocator);

  // Splits `text` into lines of at most kLineChars characters (words stay whole unless one is longer than a line),
  // at most `max_lines` of them; the last ends in "..." if text was left out. Returns the line count.
  static uint32_t wrap(StringView text, StringView* lines, uint32_t max_lines, String* storage);

  // Opens it modally in the middle of the manager's area. Returns false if it could not be added.
  bool show(WindowManager& manager, NoticeKind kind, StringView message);
  // Convenience: shows a notice on `dialog` if there is one (and a manager to show it in). False if not shown.
  static bool post(NoticeDialog* dialog, WindowManager* manager, NoticeKind kind, StringView message) {
    return dialog && manager && dialog->show(*manager, kind, message);
  }
  // Runs after the dialog was dismissed.
  void on_dismissed(Handler handler) { on_closed(handler); }
  bool showing() const { return manager() != nullptr; }
  NoticeKind kind() const { return kind_; }
  StringView message() const { return message_.view(); }
  Button& ok_button() { return ok_; }
  Label& line_label(uint32_t index) { return lines_[index]; }

  bool on_key(const KeyArgs& args) override;

 private:
  static void on_ok(void* self, Control&);

  NoticeIcon icon_;
  Label lines_[kMaxLines];
  Panel buttons_, gap_;
  Button ok_;
  NoticeKind kind_ = NoticeKind::Information;
  String message_;
  String storage_;
};

}  // namespace gui
