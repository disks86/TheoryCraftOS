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

// The Open and Save dialogs: a modal window in which the user picks a file. It shows a folder in a list (folders first, then the files
// of the chosen type), with a breadcrumb address bar above it (click a folder to go back there, or type a path), a "File name" box
// and a "Files of type" list. Double-click a folder to open it; a click on a file puts its name in the box; Open/Save (or
// Enter, or a double click on a file) accepts the name. The Open dialog only accepts files that exist; the Save dialog adds the
// type's first extension to a name that has none and asks before replacing a file. Cancel, Escape or the close button
// leave accepted() false. One dialog serves every program: show() is given the handler to run once the dialog is gone.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/BreadcrumbBar.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/DropDown.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class FileDialog : public gui::Frame {
 public:
  enum class Mode : uint8_t { Open, Save };
  // A kind of file: its name in the "Files of type" list and its extensions without dots, separated by ';' ("txt;md"); "*" is every file.
  struct Filter {
    const char* name;
    const char* extensions;
  };
  static constexpr uint32_t kMaxFilters = 8;
  static constexpr int32_t kWidth = 520, kHeight = 380;

  explicit FileDialog(allocators::IAllocator& allocator);

  // The filesystem to pick from (nullptr: none). It must outlive the dialog.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }

  // Opens the dialog modally in `manager`. `start_path` is a folder or a file: its folder is shown and its name (if it names a file or
  // is not an existing folder) is put in the name box. `result` runs once the dialog has closed, accepted or not. False if the
  // dialog is already showing or there is no filesystem.
  bool show(gui::WindowManager& manager, Mode mode, StringView title, const Filter* filters, uint32_t filter_count, StringView start_path,
            gui::Handler result);
  bool showing() const { return manager() != nullptr; }

  // The outcome (valid once the result handler runs): whether a file was picked, and its full path.
  bool accepted() const { return accepted_; }
  StringView selected_path() const { return selected_.view(); }
  Mode mode() const { return mode_; }

  // The folder being shown.
  StringView folder() const { return folder_.view(); }
  // Shows another folder; false (the old one stays) if it cannot be read.
  bool open_folder(StringView path);
  // Does what the Open / Save button does with the name in the box.
  void accept_name();

  gui::BreadcrumbBar& address_bar() { return bar_; }
  gui::ListView& list() { return list_; }
  gui::TextBox& name_box() { return name_; }
  gui::DropDown& type_box() { return type_; }
  gui::Button& ok_button() { return ok_; }
  gui::Button& cancel_button() { return cancel_; }
  gui::Label& message_label() { return message_; }
  gui::MessageDialog& replace_dialog() { return replace_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_navigate(void* self, gui::Control&);
  static void on_selection(void* self, gui::Control&);
  static void on_activated(void* self, gui::Control&);
  static void on_type(void* self, gui::Control&);
  static void on_ok(void* self, gui::Control&);
  static void on_cancel(void* self, gui::Control&);
  static void on_replace_answer(void* self, gui::Control&);
  void finish(StringView path);
  void say(StringView text) { message_.set_text(text); }
  StringView current_extensions() const;
  bool join(StringView name, String* out) const;

  gui::Panel address_row_, name_row_, type_row_, button_row_, gap_;
  gui::BreadcrumbBar bar_;
  gui::ListView list_;
  gui::Label name_label_, type_label_, message_;
  gui::TextBox name_;
  gui::DropDown type_;
  gui::Button ok_, cancel_;
  gui::MessageDialog replace_;
  filesystem::IFileSystem* fs_ = nullptr;
  Mode mode_ = Mode::Open;
  Filter filters_[kMaxFilters] = {};
  uint32_t filter_count_ = 0;
  String folder_, selected_, pending_;
  bool accepted_ = false;
};

}  // namespace desktop
