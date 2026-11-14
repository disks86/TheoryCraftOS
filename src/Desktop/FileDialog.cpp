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

#include "Desktop/FileDialog.hpp"
#include "Desktop/DirectoryListing.hpp"
#include "Desktop/FileStatus.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Desktop/Format.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/Move.hpp"

namespace desktop {

using namespace gui;
using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::Status;

namespace {

// "/a/b" from "a//b/" and the like ("/" for nothing); leading and trailing blanks go.
bool normalize(StringView in, String* out) {
  out->clear();
  uint64_t from = 0, to = in.size();
  while (from < to && in[from] == ' ') ++from;
  while (to > from && in[to - 1] == ' ') --to;
  StringView rest = in.substr(from, to - from);
  bool any = false;
  while (!rest.empty()) {
    const uint64_t slash = rest.find('/');
    const uint64_t length = slash == StringView::npos ? rest.size() : slash;
    if (length > 0) {
      if (!out->push_back('/') || !out->append(rest.first(length))) return false;
      any = true;
    }
    rest = slash == StringView::npos ? StringView() : rest.without_prefix(slash + 1);
  }
  return any || out->assign("/");
}

// Whether a name ends in what reads like an extension the user typed on purpose: a full stop followed by one to five letters and digits,
// at least one of them a letter, and not at the start of the name.
bool typed_extension(StringView name) {
  const uint64_t dot = name.rfind('.');
  if (dot == StringView::npos || dot == 0) return false;
  const StringView ext = name.substr(dot + 1);
  if (ext.empty() || ext.size() > 5) return false;
  bool letter = false;
  for (char c : ext) {
    const bool is_letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!is_letter && !(c >= '0' && c <= '9')) return false;
    letter = letter || is_letter;
  }
  return letter;
}

}  // namespace

FileDialog::FileDialog(allocators::IAllocator& allocator)
    : Frame(allocator),
      body_(allocator),
      address_row_(allocator),
      name_row_(allocator),
      type_row_(allocator),
      gap_(allocator),
      button_row_(allocator),
      bar_(allocator),
      list_(allocator),
      look_label_(allocator),
      name_label_(allocator),
      type_label_(allocator),
      message_(allocator),
      name_(allocator),
      type_(allocator),
      ok_(allocator),
      cancel_(allocator),
      replace_(allocator),
      folder_(allocator),
      selected_(allocator),
      pending_(allocator) {
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_escape_closes(true);
  set_padding(Insets::all(8));

  // From the top, in reading order (which is also the order Tab takes): the folder, what is in it, the name, the type, the buttons.
  address_row_.set_style(PanelStyle::Transparent);
  address_row_.set_size(0, 30);
  address_row_.set_padding(Insets{0, 0, 0, 4});
  look_label_.set_text("Look in:");
  look_label_.set_access_key('l');   // (pinned, so that the same letters lead to the same fields in Open and in Save As)
  look_label_.set_dock(DockStyle::Left);
  look_label_.set_size(120, 0);
  bar_.set_dock(DockStyle::Fill);
  bar_.on_navigate({&FileDialog::on_navigate, this});
  address_row_.add(&look_label_);
  address_row_.add(&bar_);

  list_.add_column("Name", 280);
  list_.add_column("Size", 80, Align::Right);
  list_.add_column("Type", 110);
  list_.set_dock(DockStyle::Fill);
  list_.set_copy_enabled(false);
  list_.on_selection_changed({&FileDialog::on_selection, this});
  list_.on_item_activated({&FileDialog::on_activated, this});

  name_row_.set_style(PanelStyle::Transparent);
  name_row_.set_size(0, 30);
  name_row_.set_padding(Insets{0, 2, 0, 2});
  name_label_.set_text("File name:");
  name_label_.set_access_key('n');
  name_label_.set_dock(DockStyle::Left);
  name_label_.set_size(120, 0);
  name_.set_dock(DockStyle::Fill);
  name_.on_changed(Handler::immediate(&FileDialog::on_name_changed, this));
  name_row_.add(&name_label_);
  name_row_.add(&name_);

  type_row_.set_style(PanelStyle::Transparent);
  type_row_.set_size(0, 30);
  type_row_.set_padding(Insets{0, 2, 0, 2});
  type_label_.set_text("Files of type:");
  type_label_.set_access_key('t');
  type_label_.set_dock(DockStyle::Left);
  type_label_.set_size(120, 0);
  type_.set_dock(DockStyle::Fill);
  type_.on_selection_changed({&FileDialog::on_type, this});
  type_row_.add(&type_label_);
  type_row_.add(&type_);

  // The buttons, right-aligned: the default one first, then Cancel; both the same width. (The message line fills what is left of them.)
  button_row_.set_size(0, 32);
  button_row_.set_padding(Insets{0, 4, 0, 0});
  message_.set_dock(DockStyle::Fill);
  ok_.set_size(92, 0);
  ok_.set_default(true);
  ok_.on_click({&FileDialog::on_ok, this});
  gap_.set_style(PanelStyle::Transparent);
  gap_.set_size(8, 0);
  cancel_.set_text("Cancel");
  cancel_.set_size(92, 0);
  cancel_.on_click({&FileDialog::on_cancel, this});
  button_row_.add(&message_);
  button_row_.add(&ok_);
  button_row_.add(&gap_);
  button_row_.add(&cancel_);

  replace_.on_result({&FileDialog::on_replace_answer, this});

  body_.set_dock(DockStyle::Fill);
  body_.add(&address_row_);
  body_.add(&list_);
  body_.add(&name_row_);
  body_.add(&type_row_);
  body_.add(&button_row_);
  add(&body_);
}

void FileDialog::say(StringView text) {
  if (text.empty()) {
    message_.set_text(text);
    return;
  }
  if (Window* w = window()) w->set_focus(&name_);
  name_.select_all();
  if (!gui::NoticeDialog::post(notices_, manager(), gui::NoticeKind::Error, text, title())) message_.set_text(text);
}

bool FileDialog::folder_selected() const {
  const int32_t item = list_.selected_item();
  return item >= 0 && list_.item_icon(static_cast<uint32_t>(item)) == ItemIcon::Folder;
}

void FileDialog::update_ok() { ok_.set_enabled(!name_.text().trimmed().empty() || folder_selected()); }

void FileDialog::on_name_changed(void* self, Control&) {   // typing takes the complaint about the old name away
  auto* d = static_cast<FileDialog*>(self);
  d->message_.set_text(StringView());
  d->update_ok();
}

// --- Showing ---------------------------------------------------------------------------

bool FileDialog::show(WindowManager& manager, Mode mode, StringView title, const Filter* filters, uint32_t filter_count, StringView start_path,
                      Handler result) {
  UiGuard lock(&manager);
  if (showing() || !fs_) return false;
  mode_ = mode;
  accepted_ = false;
  selected_.clear();
  set_title(title);
  ok_.set_text(mode == Mode::Open ? "Open" : "Save");
  look_label_.set_text(mode == Mode::Open ? "Look in:" : "Save in:");
  filter_count_ = filter_count < kMaxFilters ? filter_count : kMaxFilters;
  type_.clear_items();
  for (uint32_t i = 0; i < filter_count_; ++i) {
    filters_[i] = filters[i];
    type_.add_item(filters[i].name);
  }
  if (filter_count_ == 0) {
    filters_[0] = {"All Files", "*"};
    filter_count_ = 1;
    type_.add_item("All Files");
  }
  // A file path starts in its folder with its name filled in; a folder path just starts there.
  // A name without a folder (a document that was never saved) or nothing at all starts where the dialog was last, for the whole session;
  // the first time that is the user's documents folder.
  String start(allocator());
  DirectoryEntry entry;
  String folder(allocator()), name(allocator());
  const StringView given = start_path.trimmed();
  if (given.empty() || given[0] != '/') {
    const StringView documents(filesystem::layout::kDocuments);
    if (folder_.size() > 0) folder.assign(folder_.view());
    else folder.assign(fs_->stat(documents, &entry) == Status::Ok && entry.type == NodeType::Directory ? documents : StringView("/"));
    name.assign(given);
  } else if (normalize(start_path, &start), start.view().equals("/") || (fs_->stat(start.view(), &entry) == Status::Ok && entry.type == NodeType::Directory)) {
    folder.assign(start.view());
  } else {
    const uint64_t slash = start.view().rfind('/');
    folder.assign(slash == StringView::npos || slash == 0 ? StringView("/") : start.view().first(slash));
    name.assign(slash == StringView::npos ? start.view() : start.view().substr(slash + 1));
  }
  // Pick the filter that fits the name, if the name has an extension of one of them.
  int32_t chosen = 0;
  for (uint32_t i = 0; i < filter_count_ && !name.empty(); ++i)
    if (!StringView(filters_[i].extensions).equals("*") && name_has_extension(name.view(), filters_[i].extensions)) {
      chosen = static_cast<int32_t>(i);
      break;
    }
  type_.set_selected_index(chosen);
  if (!open_folder(folder.view()) && !open_folder("/")) return false;
  name_.set_text(name.view());
  name_.select_all();   // (typing replaces the name that was offered)
  say(StringView());
  update_ok();
  on_closed(result);
  const int32_t area_w = static_cast<int32_t>(manager.dialog_area().width), area_h = static_cast<int32_t>(manager.dialog_area().height);
  const int32_t w = kWidth < area_w ? kWidth : area_w, h = kHeight < area_h ? kHeight : area_h;
  set_resizable(false);
  set_bounds({manager.dialog_area().x + ((area_w - w) / 2 > 0 ? (area_w - w) / 2 : 0), manager.dialog_area().y + ((area_h - h) / 3 > 0 ? (area_h - h) / 3 : 0), static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
  if (!manager.add_window(this, /*modal=*/true)) return false;
  if (Window* win = window()) {
    win->layout();
    win->set_focus(&name_);
  }
  return true;
}

StringView FileDialog::current_extensions() const {
  const int32_t index = type_.selected_index();
  return index >= 0 && static_cast<uint32_t>(index) < filter_count_ ? StringView(filters_[index].extensions) : StringView("*");
}

bool FileDialog::open_folder(StringView path) {
  if (!fs_) return false;
  List<DirectoryEntry> entries(allocator());
  String next(allocator());
  if (!normalize(path, &next) || read_sorted_directory(*fs_, next.view(), &entries) != Status::Ok) return false;
  folder_ = util::move(next);
  list_.clear_items();
  const StringView extensions = current_extensions();
  String size(allocator()), type(allocator());
  for (const DirectoryEntry& e : entries) {
    const bool folder = e.type == NodeType::Directory;
    if (!folder && !name_has_extension(e.name.view(), extensions)) continue;
    if (folder) size.clear();
    else format_size(e.size, &size);
    describe_entry_type(e, &type);
    const StringView cells[3] = {e.name.view(), size.view(), type.view()};
    list_.add_item(cells, 3, folder ? ItemIcon::Folder : ItemIcon::File);
  }
  bar_.set_path(folder_.view());
  return true;
}

// The full path for a name typed in the box: absolute names stand, others are inside the folder being shown.
bool FileDialog::join(StringView name, String* out) const {
  String text(allocator());
  uint64_t from = 0;
  while (from < name.size() && name[from] == ' ') ++from;
  if (from < name.size() && name[from] == '/') return normalize(name, out);
  if (!text.assign(folder_.view()) || (folder_.size() > 1 && !text.push_back('/')) || !text.append(name.substr(from))) return false;
  return normalize(text.view(), out);
}

// --- Choosing ---------------------------------------------------------------------------

void FileDialog::finish(StringView path) {
  selected_.assign(path);
  accepted_ = true;
  close();
}

// Goes into the folder selected in the list.
void FileDialog::open_selected_folder() {
  const int32_t item = list_.selected_item();
  if (item < 0) return;
  String name(allocator()), path(allocator()), message(allocator());
  list_.item_text(static_cast<uint32_t>(item), 0, &name);
  if (!path.assign(folder_.view()) || (folder_.size() > 1 && !path.push_back('/')) || !path.append(name.view())) return;
  if (open_folder(path.view())) {
    say(StringView());
  } else {
    format_message(&message, "\"%1\" could not be opened. It may have been removed, or you may not be allowed to look in it.", name.view());
    say(message.view());
  }
  update_ok();
}

void FileDialog::accept_name() {
  String name(allocator()), message(allocator());
  name.assign(name_.text());
  if (name.view().find('/') == StringView::npos) {  // no folders in it: trim blanks only
    String trimmed(allocator());
    trimmed.assign(name.view().trimmed());
    name = util::move(trimmed);
  }
  if (name.empty()) {   // (the button is unavailable then, unless a folder is selected: Open goes into it)
    if (folder_selected()) open_selected_folder();
    return;
  }
  String full(allocator());
  if (!join(name.view(), &full)) {
    format_message(&message, "\"%1\" cannot be used as a file name. Type another name.", name.view());
    say(message.view());
    return;
  }
  DirectoryEntry entry;
  Status status = fs_->stat(full.view(), &entry);
  if (status == Status::Ok && entry.type == NodeType::Directory) {  // a folder name goes into the folder
    if (open_folder(full.view())) {
      name_.set_text(StringView());
      say(StringView());
      update_ok();
    } else {
      format_message(&message, "\"%1\" could not be opened. It may have been removed, or you may not be allowed to look in it.", name.view());
      say(message.view());
    }
    return;
  }
  if (mode_ == Mode::Open) {
    if (status == Status::Ok) {
      finish(full.view());
    } else {
      if (status == Status::NotFound) format_message(&message, "\"%1\" could not be found. Check the name, or pick the file from the list.", name.view());
      else describe_failure(&message, "\"%1\" could not be opened because %2.", name.view(), status);
      say(message.view());
    }
    return;
  }
  // Save: a name gets the type's first extension unless it already ends in one - an extension of one of the types offered, or something
  // that reads like one ("data.bin"). A full stop inside a name ("Report v1.2", "Mr. Smith") is not an extension the user meant.
  const StringView extension = first_extension(current_extensions());
  const StringView last = full.view().substr(full.view().rfind('/') + 1);
  bool has_extension = typed_extension(last);
  for (uint32_t i = 0; i < filter_count_ && !has_extension; ++i)
    has_extension = !StringView(filters_[i].extensions).equals("*") && name_has_extension(last, filters_[i].extensions);
  if (status == Status::NotFound && !extension.empty() && !has_extension) {
    if (!full.push_back('.') || !full.append(extension)) return;
    status = fs_->stat(full.view(), &entry);
  }
  if (status == Status::Ok && entry.type == NodeType::Directory) {
    format_message(&message, "\"%1\" is the name of a folder here. Type another name.", full.view().substr(full.view().rfind('/') + 1));
    say(message.view());
    return;
  }
  if (status == Status::Ok) {  // it exists: ask before replacing it, and make leaving it alone the default
    pending_ = util::move(full);
    const StringView shown = pending_.view().substr(pending_.view().rfind('/') + 1);
    format_message(&message, "\"%1\" already exists.", shown.size() > 24 ? shown.first(24) : shown);
    replace_.set_texts(shown, message.view(), "Do you want to replace it?", "Replace", "Cancel");
    replace_.set_default_choice(MessageDialog::Choice::Decline);
    if (WindowManager* m = manager()) replace_.show(*m);
    return;
  }
  const uint64_t slash = full.view().rfind('/');
  if (slash != StringView::npos && slash > 0) {  // the folder has to exist
    DirectoryEntry parent;
    if (fs_->stat(full.view().first(slash), &parent) != Status::Ok || parent.type != NodeType::Directory) {
      format_message(&message, "The folder for \"%1\" does not exist. Pick a folder from the list, then type the name.", name.view());
      say(message.view());
      return;
    }
  }
  finish(full.view());
}

void FileDialog::on_replace_answer(void* self, Control&) {
  auto* d = static_cast<FileDialog*>(self);
  if (d->replace_.accepted()) {
    d->finish(d->pending_.view());
  } else {   // back to the name, ready to be typed over
    if (Window* w = d->window()) w->set_focus(&d->name_);
    d->name_.select_all();
  }
}

void FileDialog::on_navigate(void* self, Control&) {
  auto* d = static_cast<FileDialog*>(self);
  String target(d->allocator()), message(d->allocator());
  target.assign(d->bar_.requested_path());
  if (d->open_folder(target.view())) {
    d->say(StringView());
  } else {
    format_message(&message, "\"%1\" could not be opened. It may have been removed, or you may not be allowed to look in it.", target.view());
    d->say(message.view());
  }
  d->update_ok();
}

void FileDialog::on_selection(void* self, Control&) {  // a click on a file puts its name in the box
  auto* d = static_cast<FileDialog*>(self);
  const int32_t item = d->list_.selected_item();
  if (item >= 0 && d->list_.item_icon(static_cast<uint32_t>(item)) != ItemIcon::Folder) {
    String name(d->allocator());
    d->list_.item_text(static_cast<uint32_t>(item), 0, &name);
    d->name_.set_text(name.view());
    d->say(StringView());
  }
  d->update_ok();
}

void FileDialog::on_activated(void* self, Control&) {  // Enter or a double click: into a folder, or take the file
  auto* d = static_cast<FileDialog*>(self);
  const int32_t item = d->list_.selected_item();
  if (item < 0) return;
  if (d->list_.item_icon(static_cast<uint32_t>(item)) == ItemIcon::Folder) {
    d->open_selected_folder();
    return;
  }
  String name(d->allocator());
  d->list_.item_text(static_cast<uint32_t>(item), 0, &name);
  d->name_.set_text(name.view());
  d->accept_name();
}

void FileDialog::on_type(void* self, Control&) {  // another type: show its files
  auto* d = static_cast<FileDialog*>(self);
  String folder(d->allocator());
  folder.assign(d->folder_.view());
  d->open_folder(folder.view());
  d->update_ok();
}

void FileDialog::on_ok(void* self, Control&) { static_cast<FileDialog*>(self)->accept_name(); }
void FileDialog::on_cancel(void* self, Control&) { static_cast<FileDialog*>(self)->close(); }

bool FileDialog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {
    accept_name();
    return true;
  }
  if (args.usage == hardware::key::kBackspace && window() && window()->focus() != &name_ && window()->focus() != &bar_.edit_box()) {
    String parent(allocator());  // Backspace in the list goes up a folder
    const uint64_t slash = folder_.view().rfind('/');
    if (folder_.size() > 1) {
      parent.assign(slash == 0 || slash == StringView::npos ? StringView("/") : folder_.view().first(slash));
      open_folder(parent.view());
      update_ok();
    }
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace desktop
