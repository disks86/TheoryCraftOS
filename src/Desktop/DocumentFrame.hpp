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

// What the document applications (the text editor, Paint, the spreadsheet, the database query window) have in common, as the interface guidelines
// ask for it (docs/UserInterfaceGuidelines.md, 6.3, 7.1, 11.8):
//  - The title is "Document name - Application". A document that has not been saved yet is named by its type and a number ("Document 1"), and
//    that name is also what Save As offers.
//  - A command that would lose unsaved changes - New, Open, a file dropped on the frame, closing the frame - first asks "Save" / "Don't Save" /
//    "Cancel" (gui::MessageDialog) and carries on when the answer allows it: after the document was saved (also when that needed the Save As
//    dialog first), or at once for "Don't Save". "Cancel", a save that fails and a Save As that is cancelled leave everything as it was.
//  - Warnings, information and errors go to the shared NoticeDialog (or the status line when there is none to show them in).
//  - Help > About says what the application is.
// An application derives from DocumentFrame, says whether its document is modified, how to save it and how to carry out a command once nothing
// is in the way, and tells the frame when a save worked or did not.
#pragma once
#include "Desktop/Format.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

// Why a file could not be opened or saved and what to do about it, as whole sentences that follow the one that says what happened.
inline const char* file_advice(filesystem::Status status, bool saving) {
  using filesystem::Status;
  switch (status) {
    case Status::NotFound: return saving ? "The folder it should go in no longer exists. Save it in another folder." : "It is no longer there. It may have been moved, renamed or deleted.";
    case Status::ReadOnly: return "The disk cannot be written to. Save it on another disk.";
    case Status::NoSpace: return "The disk is full. Save it on another disk, or free some space and try again.";
    case Status::IsADirectory: return saving ? "A folder already has that name. Save it under another name." : "That is a folder, not a file.";
    case Status::NotADirectory:
    case Status::InvalidPath: return saving ? "That name cannot be used. Save it under another name." : "Its name is not one that can be used here.";
    case Status::OutOfMemory: return "There is not enough memory. Close something you do not need and try again.";
    case Status::Busy: return "It is in use by something else. Try again when that has finished.";
    case Status::NoMedia: return "The disk it is on has been removed. Put it back and try again.";
    case Status::AccessDenied: return saving ? "You are not allowed to change it. Save it under another name." : "You are not allowed to read it.";
    case Status::LoginRequired:
    case Status::LoginFailed: return "The server did not accept the user name or password. Connect to it again in the file explorer.";
    case Status::UntrustedHost:
    case Status::Network: return "The server it is on could not be reached. Check the network and try again.";
    default: return saving ? "The disk reported a problem. Try again, or save it somewhere else." : "The disk reported a problem. Try again.";
  }
}

class DocumentFrame : public gui::Frame {
 public:
  // The dialog that warnings, information and errors are shown in (nullptr: the status line says them instead). It must outlive the frame.
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; }

  // The file the document came from or was saved to (empty: none yet).
  StringView path() const { return path_.view(); }
  // Its name as the title shows it: the file's name, or the type and a number for a document that has no file yet.
  StringView document_name() const { return name_.view(); }
  StringView application_name() const { return application_; }
  // The Save / Don't Save / Cancel question (showing() while it waits for an answer).
  gui::MessageDialog& save_question() { return ask_; }

 protected:
  // What waits for the answer to the question.
  enum class Next : uint8_t { None, New, Open, OpenFile, Close };

  // `application` and `type` are the names in the title ("Text Editor", "Document"); `counter` numbers the new documents of the application.
  DocumentFrame(allocators::IAllocator& allocator, const char* application, const char* type, uint32_t* counter)
      : Frame(allocator), application_(application), type_(type), counter_(counter), ask_(allocator), path_(allocator), name_(allocator), next_path_(allocator) {
    ask_.on_result({&DocumentFrame::on_answer, this});
    name_new_document();
  }

  virtual bool document_modified() const = 0;
  // The Save command. True if the document is saved now; false if it is not, or not yet (the Save As dialog is open). Whichever way a save ends,
  // the application reports it with document_saved() or save_abandoned().
  [[nodiscard]] virtual bool save_document() = 0;
  // Does what was asked for, now that no unsaved changes are in the way: New starts an empty document, Open shows the Open dialog and OpenFile
  // reads `path`. (Close is done here.)
  virtual void carry_out(Next next, StringView path) = 0;
  // One line in the status bar.
  virtual void say(StringView text) = 0;

  // True if `next` can go ahead at once. False if the document has unsaved changes: the question is then on the screen and carry_out() runs
  // when the answer allows it. (A frame that is not shown has nobody to ask and nothing the user could miss.)
  [[nodiscard]] bool ready_for(Next next, StringView path = StringView()) {
    if (!document_modified() || !manager()) return true;
    if (!ask_.showing()) {
      next_ = next;
      next_path_.assign(path);
      ask_.set_save_changes(name_.view());
      if (!ask_.show(*manager())) next_ = Next::None;
    }
    return false;
  }
  // A save worked: what waited for it goes ahead.
  void document_saved() {
    if (!saving_for_next_) return;
    saving_for_next_ = false;
    const Next next = next_;
    next_ = Next::None;
    go(next, false);
  }
  // A save failed or the Save As dialog was cancelled: what waited for it is dropped.
  void save_abandoned() {
    next_ = Next::None;
    saving_for_next_ = false;
  }

  // The document is the file at `path` from now on / is a new one without a file.
  void name_document(StringView path) {
    path_.assign(path);
    const uint64_t slash = path.rfind('/');
    name_.assign(slash == StringView::npos ? path : path.substr(slash + 1));
    update_title();
  }
  void name_new_document() {
    path_.clear();
    name_.assign(type_);
    name_.push_back(' ');
    name_.append_uint(counter_ ? __atomic_add_fetch(counter_, 1, __ATOMIC_RELAXED) : 1);
    update_title();
  }
  void update_title() {
    String title(allocator());
    format_message(&title, "%1 - %2", name_.view(), application_);
    if (!title.view().equals(this->title())) set_title(title.view());
  }

  void notify(gui::NoticeKind kind, StringView text) {
    if (!gui::NoticeDialog::post(notices_, manager(), kind, text)) say(text);
  }
  // The same for a message with the document's name (and a second piece) in it: "\"%1\" could not be opened. %2".
  void notify(gui::NoticeKind kind, StringView pattern, StringView first, StringView second = StringView()) {
    String message(allocator());
    format_message(&message, pattern, first, second);
    notify(kind, message.view());
  }
  // Help > About: `what` is a sentence that says what the application is for.
  void show_about(StringView what) {
    String title(allocator());
    format_message(&title, "About %1", application_);
    if (!gui::NoticeDialog::post(notices_, manager(), gui::NoticeKind::Information, what, title.view())) say(what);
  }
  // The name of the file in a path, for messages: the user knows the document by its name, not by where it is.
  static StringView name_of(StringView path) {
    const uint64_t slash = path.rfind('/');
    return slash == StringView::npos ? path : path.substr(slash + 1);
  }
  // Where the Open and Save As dialogs start. A document that has a file starts there. One that has none starts in `folder` (the user's
  // documents or pictures; the root while that folder does not exist), and Save As also offers the document's name with `extension` (".txt").
  bool dialog_start(filesystem::IFileSystem* fs, const char* folder, const char* extension, bool saving, String* out) const {
    if (!path_.empty()) return out->assign(path_.view());
    filesystem::DirectoryEntry entry;
    const bool there = fs && fs->stat(folder, &entry) == filesystem::Status::Ok && entry.type == filesystem::NodeType::Directory;
    if (!out->assign(there ? StringView(folder) : StringView("/"))) return false;
    if (!saving) return true;
    return (there ? out->push_back('/') : true) && out->append(name_.view()) && out->append(extension);
  }
  // File > Exit and the close button: the frame closes unless unsaved changes are in the way (then the question comes first).
  bool close_requested() override { return closing_ || ready_for(Next::Close); }
  // File > Exit. (Asked here as well as in close(): a frame that is a tab of a tabbed window must not lose its changes either.)
  void exit_requested() {
    if (close_requested()) close();
  }

 private:
  static void on_answer(void* self, gui::Control&) {
    auto* f = static_cast<DocumentFrame*>(self);
    const Next next = f->next_;
    switch (f->ask_.choice()) {
      case gui::MessageDialog::Choice::Accept:   // Save: what waited goes ahead from document_saved()
        f->saving_for_next_ = true;
        static_cast<void>(f->save_document());
        break;
      case gui::MessageDialog::Choice::Alternate:   // Don't Save
        f->next_ = Next::None;
        f->go(next, true);
        break;
      default:
        f->next_ = Next::None;
        break;
    }
  }
  void go(Next next, bool discard) {
    if (next == Next::None) return;
    if (next != Next::Close) {
      carry_out(next, next_path_.view());
      return;
    }
    closing_ = true;
    close();
    closing_ = false;
    if (discard) carry_out(Next::New, StringView());   // the frame may be opened again: it then starts with an empty document, not with what was thrown away
  }

  const char* application_;
  const char* type_;
  uint32_t* counter_;
  gui::NoticeDialog* notices_ = nullptr;
  gui::MessageDialog ask_;
  String path_, name_;
  Next next_ = Next::None;
  String next_path_;
  bool saving_for_next_ = false, closing_ = false;
};

}  // namespace desktop
