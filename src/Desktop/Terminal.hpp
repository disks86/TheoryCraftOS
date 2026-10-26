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

// The terminal application: a window with a menu bar (File: New Terminal, Save Output As, Close; Edit: Copy All, Paste, Clear) around a
// TerminalView, with a Shell behind it that runs the commands typed. The title shows the current folder. A folder dragged from a file
// explorer and dropped on the window becomes the current folder (as if "cd" had been typed); a file dropped there has its path put on the
// command line. Ctrl+N, Ctrl+S, Ctrl+V and Ctrl+L do the same as the menu items. Commands run on the window's handler thread.
#pragma once
#include "Desktop/FileDialog.hpp"
#include "Desktop/Shell.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/TerminalView.hpp"
#include "Executable/ExecutableKind.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class Terminal : public gui::Frame {
 public:
  explicit Terminal(allocators::IAllocator& allocator);

  // The filesystem the shell works on (nullptr: none). It must outlive the terminal. The current folder goes back to the root.
  void set_file_system(filesystem::IFileSystem* fs);
  // The dialog that Save Output As uses (nullptr: none). It must outlive the terminal.
  void set_file_dialog(FileDialog* dialog) { dialog_ = dialog; }
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; }
  // A path in the output that exists - absolute, or relative to the current folder - can be clicked; this fires with opened_path() set to its full
  // path (the desktop opens a folder in a file explorer and a file in the program for it).
  void on_open_path(gui::Handler handler) { open_path_ = handler; }
  StringView opened_path() const { return opened_path_.view(); }
  // Fired by File > New Terminal; the desktop opens another window.
  void on_new_terminal(gui::Handler handler) { new_terminal_ = handler; }

  // Makes `path` the current folder and shows the "cd" in the output; false (with the reason printed) if it is not a folder.
  bool change_directory(StringView path);
  // Runs a command line as if it had been typed: it is echoed with the prompt, then run.
  void run(StringView line);
  // Writes the output to `path`. False (with a notice) if that fails.
  bool save_output(StringView path);
  void ask_save_output();

  gui::TerminalView& view() { return view_; }
  Shell& shell() { return shell_; }
  gui::MenuBar& menu_bar() { return menu_; }

  // A path dragged here from a file explorer: a folder is entered, a file's path is put on the command line.
  bool accepts_drop(StringView text, int32_t, int32_t, const gui::Control*) const override { return fs_ && !text.empty() && text[0] == '/'; }
  void on_drop(StringView text, int32_t x, int32_t y, const gui::Control* source) override;
  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_new(void* self, gui::Control&);
  static void on_save(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  static void on_copy_all(void* self, gui::Control&);
  static void on_paste(void* self, gui::Control&);
  static void on_clear(void* self, gui::Control&);
  static void on_submitted(void* self, gui::Control&);
  static bool is_link(void* self, StringView word);
  static void on_link(void* self, gui::Control&);
  static void on_save_picked(void* self, gui::Control&);
  static void host_write(void* self, StringView text);
  static void host_write_output(void* self, StringView text);
  static void host_clear(void* self);
  static void host_exit(void* self);
  static void host_unsupported_executable(void* self, StringView path, exe::ExecutableKind kind);
  static bool host_run_executable(void* self, StringView path, exe::ExecutableKind kind, ArrayView<const StringView> arguments);
  void refresh_prompt();
  void notify(gui::NoticeKind kind, StringView text) { gui::NoticeDialog::post(notices_, manager(), kind, text); }

  gui::MenuBar menu_;
  gui::TerminalView view_;
  Shell shell_;
  filesystem::IFileSystem* fs_ = nullptr;
  FileDialog* dialog_ = nullptr;
  gui::NoticeDialog* notices_ = nullptr;
  gui::Handler new_terminal_, open_path_;
  String opened_path_;
};

}  // namespace desktop
