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

// A terminal: a black screen of fixed-width characters that shows text written to it (write()) - output that scrolls up into a scrollback of
// up to kMaxLines lines and wraps at the right edge - and, after it, the prompt and the line being typed. The control only edits the line: Enter
// hands it to the program (on_submit; what to do with it, say run it as a command, is the program's business) and keeps it in the history,
// which the Up and Down keys walk through. Left, Right, Home, End, Backspace and Delete edit the line, Page Up / Page Down and the mouse wheel
// scroll back through the output (typing jumps down to the end again), and Ctrl+V pastes the first line of the clipboard. Characters other than
// the printable ASCII ones are dropped or shown as '?', and a tab moves to the next multiple of 8 columns. write() may be called from any thread.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Controls/ScrollBar.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace gui {

class TerminalView : public Control {
 public:
  static constexpr uint32_t kMaxLines = 2000, kMaxInput = 1024, kMaxHistory = 64;
  static constexpr int32_t kMargin = 4, kLineHeight = 16;

  explicit TerminalView(allocators::IAllocator& allocator);

  // --- Output
  // Appends text to the output: '\n' ends a line, '\t' goes to the next tab stop, '\b' erases the last character, '\r' is ignored.
  void write(StringView text);
  // Forgets all output (the prompt and the line being typed stay).
  void clear();
  uint32_t line_count() const { return static_cast<uint32_t>(lines_.count()); }
  StringView line(uint32_t index) const { return index < lines_.count() ? lines_[index].view() : StringView(); }
  // All of the output as text, lines separated by '\n'. False if memory ran out.
  bool text(String* out) const;
  bool copy_all();

  // --- The line being typed
  void set_prompt(StringView prompt);
  StringView prompt() const { return prompt_.view(); }
  StringView input() const { return input_.view(); }
  uint32_t caret() const { return caret_; }
  // Replaces the line (the caret goes to its end); truncated at kMaxInput.
  void set_input(StringView text);
  // Inserts at the caret; characters that cannot be typed are skipped, and the text ends at its first line break.
  bool insert_input(StringView text);
  bool paste();
  // Enter: the line goes to the output (after the prompt) and into the history, the input is empty again, and the handler is fired.
  void submit();
  void on_submit(Handler handler) { UiGuard lock(this); submitted_handler_ = handler; }
  // The line that was submitted last.
  StringView submitted() const { return submitted_.view(); }

  // --- View
  uint32_t columns() const { return columns_; }
  uint32_t rows() const { return rows_; }
  // How many rows the view is scrolled back from the end (0: following the output).
  uint32_t scrolled_back() const { return scrolled_; }
  void scroll_to_end();

  bool focusable() const override { return true; }
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_wheel(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 private:
  void measure();                            // columns_, rows_ and the scroll bar from the size
  uint32_t rows_of(uint64_t length, bool live) const;
  uint32_t total_rows() const;
  void scroll_by(int32_t rows);
  void sync_scroll_bar();
  static void scroll_changed(void* self, Control&);
  void remember(StringView line);
  void recall(int32_t direction);

  List<String> lines_;     // the output; the last one is the line still being written
  String prompt_, input_, submitted_, saved_input_, scratch_;
  List<String> history_;
  int32_t history_at_ = -1;   // which history entry the input shows (-1: the line being typed)
  uint32_t caret_ = 0;
  uint32_t columns_ = 80, rows_ = 24;
  uint32_t scrolled_ = 0;
  uint32_t column_ = 0;       // the column the next written character goes to (for tab stops)
  bool syncing_ = false;
  ScrollBar scroll_bar_;
  Handler submitted_handler_;
  String clipboard_;          // used when the window has no system clipboard
};

}  // namespace gui
