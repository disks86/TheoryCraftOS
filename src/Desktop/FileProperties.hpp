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

// The properties of a file or folder: a window with a table of what is known about it - name, location, kind, size, what a folder holds, whether
// it can be changed - and, for pictures, videos and sounds, what the format says: the size in pixels, how many frames, the length, the frame rate,
// the sample rate and the number of channels. The format is read from the file's contents (not its name); a file that is too big to read whole is
// still looked at where the format allows it (videos are read through a byte source, only their headers). The window is reached from the file
// explorer's Properties command (Alt+Enter).
//
// It is a property sheet as the interface guidelines describe one (docs/UserInterfaceGuidelines.md, 6.3 and 11.5): modeless, titled
// "<Object name> Properties", with OK, Cancel and Apply below the properties. The property that can be changed is the name. OK applies a changed
// name and closes; Apply applies it and stays open; Cancel (and Esc) discards it and closes. The Close button is not Cancel: with a change
// that was not applied it asks whether to apply it. A name that cannot be used keeps the sheet open and puts the focus back in the name box.
#pragma once
#include "Desktop/StackPanel.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/NoticeDialog.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Button.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace desktop {

class FileProperties : public gui::Frame {
 public:
  // Files above this size are not read whole to find out their picture size or sound length.
  static constexpr uint64_t kMaxInspectBytes = 32ull * 1024 * 1024;

  explicit FileProperties(allocators::IAllocator& allocator);

  // The filesystem the files are on (nullptr: none). It must outlive the window.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }
  // The dialog that a name which cannot be used is explained in (nullptr: the line above the buttons says it). It must outlive the window.
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; }
  // Fired after a change was applied (the object was renamed): whoever shows the folder reads it again.
  void on_changed(gui::Handler handler) { changed_ = handler; }
  // Looks at the file or folder at `path` and fills the table. False (with the table saying why) if it does not exist.
  bool show(StringView path);
  StringView path() const { return path_.view(); }

  uint32_t row_count() const { return static_cast<uint32_t>(names_.count()); }
  StringView property(uint32_t row) const { return row < names_.count() ? names_[row].view() : StringView(); }
  StringView value(uint32_t row) const { return row < values_.count() ? values_[row].view() : StringView(); }
  // The value of the row called `name` (empty if there is none).
  StringView value_of(StringView name) const;
  gui::DataGrid& grid() { return grid_; }
  gui::Button& copy_button() { return copy_button_; }
  gui::TextBox& name_box() { return name_box_; }
  gui::Button& ok_button() { return ok_; }
  gui::Button& cancel_button() { return cancel_; }
  gui::Button& apply_button() { return apply_; }
  gui::Label& message_label() { return message_; }
  // The question the Close button asks when a change was not applied: "Apply" / "Don't Apply" / "Cancel".
  gui::MessageDialog& apply_question() { return ask_; }
  // Whether the name in the box differs from the object's (a change waiting for OK or Apply).
  bool pending() const;
  // Applies the pending change. False (the sheet says why, the name box has the focus) if it cannot be applied; true also when nothing is pending.
  bool apply();

  bool on_key(const gui::KeyArgs& args) override;

 protected:
  bool close_requested() override;

 private:
  static void on_copy(void* self, gui::Control&);
  static void on_ok(void* self, gui::Control&);
  static void on_cancel(void* self, gui::Control&);
  static void on_apply(void* self, gui::Control&);
  static void on_name_changed(void* self, gui::Control&);
  static void on_answer(void* self, gui::Control&);
  void close_now();
  void refuse(StringView text);
  void add_row(StringView name, StringView value);
  void clear();
  void add_folder_contents(StringView path);
  void add_media(StringView path, uint64_t size);
  bool read_whole(StringView path, uint64_t size, ByteBuffer* out);
  void add_image(ConstByteView data);
  void add_video(StringView path, ConstByteView head, uint64_t size);
  void add_audio(ConstByteView data);

  filesystem::IFileSystem* fs_ = nullptr;
  String path_;
  String name_;   // the object's name as it is now (empty: it has none that could be changed)
  StackColumn body_;   // (the controls in reading order, which is the order Tab takes)
  gui::Panel name_row_, filler_, gap_, gap2_;
  gui::Label name_label_, message_;
  gui::TextBox name_box_;
  gui::DataGrid grid_;
  StackRow buttons_;
  gui::Button copy_button_, ok_, cancel_, apply_;
  gui::MessageDialog ask_;
  gui::NoticeDialog* notices_ = nullptr;
  gui::Handler changed_;
  bool closing_ = false;   // OK, Cancel or an answered question closes without asking
  List<String> names_, values_;
};

}  // namespace desktop
