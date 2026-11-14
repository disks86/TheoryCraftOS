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

// (included by tests/UiGuideTest.cpp)
// The file explorer, the Open / Save As dialog, the file property sheet and the copy progress window against docs/UserInterfaceGuidelines.md.
#include "Desktop/CopyProgressDialog.hpp"
#include "Desktop/DirectoryListing.hpp"
#include "Desktop/FileDialog.hpp"
#include "Desktop/FileExplorer.hpp"
#include "Desktop/FileProperties.hpp"
#include "Desktop/FileStatus.hpp"

static void files_make_file(filesystem::RamFileSystem& fs, const char* path, const char* text) {
  UniquePointer<filesystem::IFile> file;
  uint64_t written = 0;
  if (fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok)
    file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text), strlen(text)), &written);
}
static bool files_exists(filesystem::RamFileSystem& fs, const char* path) {
  filesystem::DirectoryEntry entry;
  return fs.stat(path, &entry) == filesystem::Status::Ok;
}
// The middle of the row of the item called `name` in the explorer's list, in window coordinates ({-1, -1}: no such item).
static Point files_item_point(allocators::IAllocator& a, desktop::FileExplorer& e, const char* name) {
  for (uint32_t i = 0; i < e.view().item_count(); ++i) {
    String text(a);
    e.view().item_text(i, 0, &text);
    Rect2D row;
    if (text.view().equals(name) && e.view().item_rect(i, &row)) {
      const Point o = e.view().window_origin();
      return {o.x + row.x + 30, o.y + row.y + 5};
    }
  }
  return {-1, -1};
}
static int32_t files_index(allocators::IAllocator& a, desktop::FileExplorer& e, const char* name) {
  for (uint32_t i = 0; i < e.view().item_count(); ++i) {
    String text(a);
    e.view().item_text(i, 0, &text);
    if (text.view().equals(name)) return static_cast<int32_t>(i);
  }
  return -1;
}
static bool files_selected(allocators::IAllocator& a, desktop::FileExplorer& e, const char* name) {
  const int32_t i = files_index(a, e, name);
  return i >= 0 && e.view().is_selected(static_cast<uint32_t>(i));
}
// Clicks an item with modifier keys held (the window is told what is held, as the input pump tells it).
static void files_click(Window& w, allocators::IAllocator& a, desktop::FileExplorer& e, const char* name, uint8_t held = 0) {
  const Point p = files_item_point(a, e, name);
  w.set_held_modifiers(held);
  click(w, p.x, p.y);
  w.set_held_modifiers(0);
}
// The texts of a menu's items in order, separators as "|": "Open,|,Cut,Copy".
static std::string files_menu_items(Menu& m) {
  std::string out;
  for (uint32_t i = 0; i < m.item_count(); ++i) {
    if (i) out += ",";
    out += m.item(i)->separator ? std::string("|") : std::string(m.item(i)->text.view().data(), m.item(i)->text.view().size());
  }
  return out;
}
static bool files_enabled(MenuBar& bar, const char* menu, const char* item) {
  const MenuItem* i = bar.find_item(menu, item);
  return i && i->enabled();
}
struct FilesCounter {
  int count = 0;
  static void fn(void* context, Control&) { ++static_cast<FilesCounter*>(context)->count; }
  Handler handler() { return {&FilesCounter::fn, this}; }
};

static void ui_guide_files() {
  // --- Wording: reasons read as the end of a sentence, and a failure is one whole message (14.2, 11.8).
  {
    TestAllocator a;
    {
      String text(a);
      CHECK(desktop::describe_failure(&text, "\"%1\" could not be renamed because %2.", "Report", filesystem::Status::NoSpace));
      CHECK(text.view().equals("\"Report\" could not be renamed because the drive is full. Free some space, or choose another drive, and try again."));
      CHECK(desktop::describe_failure(&text, "\"%1\" could not be deleted because %2.", "a", filesystem::Status::NotEmpty) && text.view().equals("\"a\" could not be deleted because the folder is not empty."));
      // Types are named in the user's words; an unknown type shows its extension.
      filesystem::DirectoryEntry e;
      e.type = filesystem::NodeType::File;
      e.name.assign("notes.txt");
      CHECK(desktop::describe_entry_type(e, &text) && text.view().equals("Text Document"));
      e.name.assign("photo.PNG");
      CHECK(desktop::describe_entry_type(e, &text) && text.view().equals("Picture"));
      e.name.assign("data.xyz");
      CHECK(desktop::describe_entry_type(e, &text) && text.view().equals("XYZ File"));
    }
    CHECK(a.live == 0);
  }

  // --- File explorer -------------------------------------------------------------------------------------------------------------------------
  {
    TestAllocator a;
    {
      filesystem::RamFileSystem fs(a);
      CHECK(fs.create_directory("/work") == filesystem::Status::Ok && fs.create_directory("/work/sub") == filesystem::Status::Ok && fs.create_directory("/other") == filesystem::Status::Ok);
      files_make_file(fs, "/work/a.txt", "aaa");
      files_make_file(fs, "/work/b.txt", "bbb");
      files_make_file(fs, "/work/c.txt", "ccc");
      files_make_file(fs, "/work/d.txt", "ddd");
      Screen screen(a, 800, 600);
      Theme theme;
      Window w(a, screen.renderer, theme, {800, 600});
      WindowManager mgr(a);
      mgr.set_dock(DockStyle::Fill);
      w.add(&mgr);
      kernel::Clipboard cb(a);
      w.set_clipboard(&cb);
      desktop::FileExplorer ex(a);
      ex.set_file_system(&fs);
      ex.set_bounds({0, 0, 600, 400});
      CHECK(mgr.add_window(&ex));
      w.layout();
      MenuBar& bar = ex.menu_bar();

      // Menus: the standard order, access keys, Help last ending in About; File ends in Close after a separator; Edit holds the transfer commands.
      CHECK(standard_menu_order(bar) && menus_have_access_keys(ex, bar) && help_menu_ends_in_about(bar, "File Explorer"));
      CHECK(bar.menu_count() == 5 && bar.menu(0)->title().equals("File") && bar.menu(1)->title().equals("Edit") && bar.menu(2)->title().equals("View") && bar.menu(3)->title().equals("Go"));
      CHECK(files_menu_items(*bar.menu(0)) == "New Folder,New Text Document,Open,|,Rename,Properties,|,Eject,|,Close");
      CHECK(files_menu_items(*bar.menu(1)) == "Cut,Copy,Paste,|,Delete,Select All,|,Copy Path");
      CHECK(bar.find_item("Go", "Go To Address...") && bar.find_item("Go", "Connect to Server...") && bar.find_item("File", "Properties"));   // an ellipsis only where more input follows

      // The title names the folder shown, without its path; the list of drives is just the application.
      CHECK(ex.title().equals("File Explorer"));
      CHECK(ex.open_folder("/work") && ex.title().equals("work - File Explorer"));
      w.layout();
      CHECK(ex.view().item_count() == 5);   // sub, a.txt, b.txt, c.txt, d.txt

      // With nothing selected the commands that need a selection are disabled, not left to fail (they are brought up to date as a menu opens).
      w.set_focus(&ex.view());
      bar.open_menu(1);
      CHECK(!files_enabled(bar, "Edit", "Cut") && !files_enabled(bar, "Edit", "Copy") && !files_enabled(bar, "Edit", "Delete") && !files_enabled(bar, "Edit", "Paste") &&
            files_enabled(bar, "Edit", "Select All") && !files_enabled(bar, "File", "Open") && !files_enabled(bar, "File", "Rename") && files_enabled(bar, "File", "Properties") &&
            !files_enabled(bar, "File", "Eject") && files_enabled(bar, "Go", "Up One Level"));
      w.close_popup();
      CHECK(!choose(w, bar, "Edit", "Delete") && !ex.delete_dialog().showing());
      press(w, key::kDelete);
      CHECK(!ex.delete_dialog().showing());

      // Selection with the pointer (5.1): a click selects one; Ctrl+click toggles; Shift+click extends from the anchor; a click on empty space clears.
      files_click(w, a, ex, "a.txt");
      CHECK(ex.view().selection_count() == 1 && files_selected(a, ex, "a.txt"));
      files_click(w, a, ex, "c.txt", modifier::kLeftControl);
      CHECK(ex.view().selection_count() == 2 && files_selected(a, ex, "a.txt") && files_selected(a, ex, "c.txt") && !files_selected(a, ex, "b.txt"));
      CHECK(ex.status_label().text().equals("5 items  |  2 selected"));
      files_click(w, a, ex, "a.txt", modifier::kLeftControl);
      CHECK(ex.view().selection_count() == 1 && files_selected(a, ex, "c.txt"));
      files_click(w, a, ex, "a.txt");
      files_click(w, a, ex, "c.txt", modifier::kLeftShift);
      CHECK(ex.view().selection_count() == 3 && files_selected(a, ex, "a.txt") && files_selected(a, ex, "b.txt") && files_selected(a, ex, "c.txt"));
      files_click(w, a, ex, "d.txt", modifier::kLeftShift);   // the anchor does not move
      CHECK(ex.view().selection_count() == 4 && files_selected(a, ex, "a.txt") && files_selected(a, ex, "d.txt"));
      files_click(w, a, ex, "b.txt");                          // a plain click on one of several leaves just that one
      CHECK(ex.view().selection_count() == 1 && files_selected(a, ex, "b.txt"));
      {
        const Point o = ex.view().window_origin();
        click(w, o.x + 200, o.y + static_cast<int32_t>(ex.view().bounds().height) - 30);
        CHECK(ex.view().selection_count() == 0);
      }

      // Selection with the keyboard (5.2): the first arrow key selects the first item; Shift+arrow extends; a plain arrow collapses;
      // Ctrl+arrow moves the focus only and Space toggles the item with the focus.
      w.set_focus(&ex.view());
      press(w, key::kDown);
      CHECK(ex.view().selection_count() == 1 && files_selected(a, ex, "sub"));
      press(w, key::kDown);
      press(w, key::kDown, modifier::kLeftShift);
      press(w, key::kDown, modifier::kLeftShift);
      CHECK(ex.view().selection_count() == 3 && files_selected(a, ex, "a.txt") && files_selected(a, ex, "b.txt") && files_selected(a, ex, "c.txt"));
      press(w, key::kUp, modifier::kLeftShift);
      CHECK(ex.view().selection_count() == 2 && files_selected(a, ex, "a.txt") && files_selected(a, ex, "b.txt"));
      press(w, key::kUp);
      CHECK(ex.view().selection_count() == 1 && files_selected(a, ex, "a.txt"));
      press(w, key::kDown, modifier::kLeftControl);
      press(w, key::kDown, modifier::kLeftControl);
      CHECK(ex.view().selection_count() == 1 && files_selected(a, ex, "a.txt") && ex.view().selected_item() == files_index(a, ex, "c.txt"));
      press(w, key::kSpace);
      CHECK(ex.view().selection_count() == 2 && files_selected(a, ex, "a.txt") && files_selected(a, ex, "c.txt"));
      press(w, key::kSpace);
      CHECK(ex.view().selection_count() == 1 && files_selected(a, ex, "a.txt"));

      // Several selected: only the commands that apply to all of them (5.3).
      files_click(w, a, ex, "a.txt");
      files_click(w, a, ex, "b.txt", modifier::kLeftControl);
      bar.open_menu(0);
      CHECK(!files_enabled(bar, "File", "Open") && !files_enabled(bar, "File", "Rename") && !files_enabled(bar, "File", "Properties") && files_enabled(bar, "Edit", "Cut") &&
            files_enabled(bar, "Edit", "Copy") && files_enabled(bar, "Edit", "Delete"));
      w.close_popup();

      // Context menus (8.2). On the selection: a secondary click on it keeps it; the primary command first, then the transfer commands, then the
      // others, Properties last. On an unselected item: selects it first. On empty space: the commands of the folder. No shortcut texts are drawn.
      {
        Point p = files_item_point(a, ex, "b.txt");
        right_click(w, p.x, p.y);
        CHECK(w.popup() && ex.view().selection_count() == 2);
        CHECK(files_menu_items(ex.list_menu().menu()) == "Open,|,Cut,Copy,|,Rename,Delete,Copy Path,|,Properties");
        CHECK(!ex.list_menu().find_item("Open")->enabled() && ex.list_menu().find_item("Cut")->enabled() && !ex.list_menu().find_item("Properties")->enabled());
        w.close_popup();
        p = files_item_point(a, ex, "d.txt");
        right_click(w, p.x, p.y);
        CHECK(w.popup() && ex.view().selection_count() == 1 && files_selected(a, ex, "d.txt") && ex.list_menu().find_item("Open")->enabled() && ex.list_menu().find_item("Properties")->enabled());
        w.close_popup();
        const Point o = ex.view().window_origin();
        right_click(w, o.x + 200, o.y + static_cast<int32_t>(ex.view().bounds().height) - 30);
        CHECK(w.popup() && ex.view().selection_count() == 0);
        CHECK(files_menu_items(ex.list_menu().menu()) == "Paste,|,New Folder,New Text Document,|,Select All,|,List,Icons,Details,|,Up One Level,Refresh,|,Properties");
        CHECK(!ex.list_menu().find_item("Paste")->enabled() && ex.list_menu().find_item("Details")->checked());
        // Nothing is only in a context menu: every item is in the menu bar too.
        for (uint32_t i = 0; i < ex.list_menu().menu().item_count(); ++i) {
          const MenuItem* item = ex.list_menu().menu().item(i);
          bool found = item->separator;
          for (uint32_t m = 0; m < bar.menu_count() && !found; ++m) found = bar.find_item(bar.menu(m)->title(), item->text.view()) != nullptr;
          CHECK(found);
        }
        w.close_popup();
        // From the keyboard (Shift+F10) the menu is for the selection.
        files_click(w, a, ex, "a.txt");
        press(w, static_cast<uint8_t>(key::kF1 + 9), modifier::kLeftShift);
        CHECK(w.popup() && ex.list_menu().find_item("Open") && ex.view().selection_count() == 1);
        w.close_popup();
      }

      // Alt+Enter is Properties (of the selection; of the folder when nothing is selected), also when the list has the focus.
      {
        FilesCounter properties;
        ex.on_properties(properties.handler());
        files_click(w, a, ex, "a.txt");
        press(w, key::kEnter, modifier::kLeftAlt);
        CHECK(properties.count == 1 && ex.properties_path().equals("/work/a.txt"));
        CHECK(choose(w, bar, "File", "Properties") && properties.count == 2);
        ex.view().select_item(-1);
        press(w, key::kEnter, modifier::kLeftAlt);
        CHECK(properties.count == 3 && ex.properties_path().equals("/work"));
        ex.on_properties({});
      }

      // Delete asks first, and the button that leaves everything alone is the default: Enter and Esc both keep the files.
      files_click(w, a, ex, "c.txt");
      files_click(w, a, ex, "d.txt", modifier::kLeftControl);
      press(w, key::kDelete);
      CHECK(ex.delete_dialog().showing() && ex.delete_dialog().default_choice() == MessageDialog::Choice::Decline && ex.delete_dialog().decline_button().is_default() &&
            !ex.delete_dialog().accept_button().is_default() && ex.delete_dialog().accept_button().text().equals("Delete") && ex.delete_dialog().decline_button().text().equals("Cancel"));
      CHECK(ex.delete_dialog().title().equals("work - File Explorer"));
      press(w, key::kEnter);
      CHECK(!ex.delete_dialog().showing() && files_exists(fs, "/work/c.txt") && files_exists(fs, "/work/d.txt"));
      CHECK(choose(w, bar, "Edit", "Delete") && ex.delete_dialog().showing());
      press(w, key::kEscape);
      CHECK(!ex.delete_dialog().showing() && files_exists(fs, "/work/c.txt"));
      press(w, key::kDelete);
      click_center(w, ex.delete_dialog().accept_button());
      CHECK(!ex.delete_dialog().showing() && !files_exists(fs, "/work/c.txt") && !files_exists(fs, "/work/d.txt") && ex.view().item_count() == 3);

      // Cut and Paste move (the command equivalent of dragging); Copy and Paste copy, and a second copy is "Copy of ...", then "Copy 2 of ...".
      files_click(w, a, ex, "a.txt");
      ctrl(w, 'x');
      CHECK(ex.status_label().text().equals("Cut \"a.txt\". It moves when you paste it.") && files_exists(fs, "/work/a.txt"));
      CHECK(ex.open_folder("/other"));
      w.layout();
      bar.open_menu(1);
      CHECK(files_enabled(bar, "Edit", "Paste"));
      w.close_popup();
      CHECK(choose(w, bar, "Edit", "Paste"));
      CHECK(files_exists(fs, "/other/a.txt") && !files_exists(fs, "/work/a.txt") && ex.status_label().text().equals("Pasted 1 item"));
      CHECK(!ex.paste() && files_exists(fs, "/other/a.txt"));   // what was moved is not there to be pasted again
      files_click(w, a, ex, "a.txt");
      CHECK(choose(w, bar, "Edit", "Copy"));
      ctrl(w, 'v');
      ctrl(w, 'v');
      CHECK(files_exists(fs, "/other/a.txt") && files_exists(fs, "/other/Copy of a.txt") && files_exists(fs, "/other/Copy 2 of a.txt"));
      // Paste is unavailable in the list of drives and when the clipboard has no text.
      CHECK(ex.open_folder("/"));
      bar.open_menu(1);
      CHECK(!files_enabled(bar, "Edit", "Paste"));
      w.close_popup();
      CHECK(ex.open_folder("/other"));
      w.layout();
      // Text that names no file: one message that says what is the matter (no notice dialog here, so the status line carries it).
      CHECK(cb.set_text("just words") && !ex.paste());
      CHECK(ex.status_label().text().equals("The clipboard does not hold files or folders, so there is nothing to paste here."));

      // New in the container: the name comes from the type and can be typed over at once; a second one is numbered.
      CHECK(choose(w, bar, "File", "New Text Document") && files_exists(fs, "/other/New Text Document.txt") && ex.view().renaming());
      press(w, key::kEscape);
      CHECK(choose(w, bar, "File", "New Folder") && files_exists(fs, "/other/New Folder"));
      press(w, key::kEscape);
      CHECK(ex.new_folder() && files_exists(fs, "/other/New Folder 2"));
      press(w, key::kEscape);

      // Renaming (F2, or File > Rename): Enter keeps the name; a name that is taken says what happened, why and what to do.
      files_click(w, a, ex, "New Folder 2");
      press(w, static_cast<uint8_t>(key::kF1 + 1));
      CHECK(ex.view().renaming());
      ctrl(w, 'a');
      type_text(w, "Plans");
      press(w, key::kEnter);
      CHECK(files_exists(fs, "/other/Plans") && !files_exists(fs, "/other/New Folder 2") && files_selected(a, ex, "Plans"));
      CHECK(!ex.rename_item("Plans", "New Folder"));
      CHECK(ex.status_label().text().equals("\"Plans\" could not be renamed because that name is already used. Choose another name."));
      CHECK(ex.rename_item("Plans", "   ") && files_exists(fs, "/other/Plans"));   // nothing typed: the name stays, without a message

      // Help > About names the application.
      CHECK(choose(w, bar, "Help", "About File Explorer"));
      CHECK(ex.status_label().text().first(13).equals("File Explorer"));

      // Dragging: Esc cancels and nothing moves; dropped on a folder the item moves into it.
      {
        CHECK(ex.open_folder("/work"));
        w.layout();
        const Point from = files_item_point(a, ex, "b.txt"), to = files_item_point(a, ex, "sub");
        w.mouse_move(from.x, from.y);
        w.mouse_button(from.x, from.y, MouseButton::Left, true);
        w.mouse_move(to.x, to.y);
        CHECK(w.dragging());
        press(w, key::kEscape);
        CHECK(!w.dragging());
        w.mouse_button(to.x, to.y, MouseButton::Left, false);
        CHECK(files_exists(fs, "/work/b.txt") && !files_exists(fs, "/work/sub/b.txt"));
        w.mouse_move(from.x, from.y);
        w.mouse_button(from.x, from.y, MouseButton::Left, true);
        w.mouse_move(to.x, to.y);
        w.mouse_button(to.x, to.y, MouseButton::Left, false);
        CHECK(files_exists(fs, "/work/sub/b.txt") && !files_exists(fs, "/work/b.txt"));
      }

      // No colour of the interface is spelled out: the frame paints with whatever theme it is given.
      w.render();
      mgr.remove_window(&ex);
      w.set_clipboard(nullptr);
    }
    CHECK(a.live == 0);
  }

  // --- Open and Save As ----------------------------------------------------------------------------------------------------------------------
  {
    TestAllocator a;
    {
      filesystem::RamFileSystem fs(a);
      CHECK(fs.create_directory("/Desktop") == filesystem::Status::Ok && fs.create_directory(filesystem::layout::kDocuments) == filesystem::Status::Ok && fs.create_directory("/proj") == filesystem::Status::Ok &&
            fs.create_directory("/proj/deep") == filesystem::Status::Ok);
      files_make_file(fs, "/proj/a.txt", "a");
      files_make_file(fs, "/proj/pic.png", "p");
      Screen screen(a, 800, 600);
      Theme theme;
      Window w(a, screen.renderer, theme, {800, 600});
      WindowManager mgr(a);
      mgr.set_dock(DockStyle::Fill);
      w.add(&mgr);
      desktop::FileDialog dlg(a);
      dlg.set_file_system(&fs);
      using Mode = desktop::FileDialog::Mode;
      static const desktop::FileDialog::Filter kFilters[] = {{"Text Documents", "txt;md"}, {"Pictures", "png"}, {"All Files", "*"}};
      FilesCounter closed;

      // The first time, with no folder named, it starts in the user's documents folder; the title is the command's name without the ellipsis.
      CHECK(dlg.show(mgr, Mode::Open, "Open", kFilters, 3, StringView(), closed.handler()));
      CHECK(dlg.title().equals("Open") && dlg.folder().equals(filesystem::layout::kDocuments) && dlg.ok_button().text().equals("Open"));
      // The default button has the heavier outline; with nothing to open it is unavailable instead of answering with a message.
      CHECK(dlg.ok_button().is_default() && !dlg.cancel_button().is_default() && !dlg.ok_button().enabled() && w.focus() == &dlg.name_box());
      // Labels end in a colon and come directly before their fields, so their access keys lead there: Alt+N, the name; Alt+T, the type.
      dlg.assign_access_keys();
      w.set_focus(&dlg.list());
      alt(w, 'n');
      CHECK(w.focus() == &dlg.name_box());
      alt(w, 't');
      CHECK(w.focus() == &dlg.type_box());
      // Tab follows the reading order: the list, the name, the type, then the buttons with the default one first.
      CHECK(dlg.open_folder("/proj"));
      dlg.name_box().set_text("a.txt");
      w.set_focus(&dlg.list());
      press(w, key::kTab);
      CHECK(w.focus() == &dlg.name_box());
      press(w, key::kTab);
      CHECK(w.focus() == &dlg.type_box());
      press(w, key::kTab);
      CHECK(w.focus() == &dlg.ok_button());
      press(w, key::kTab);
      CHECK(w.focus() == &dlg.cancel_button());
      // Esc is Cancel.
      press(w, key::kEscape);
      CHECK(!dlg.showing() && !dlg.accepted() && closed.count == 1);

      // The folder is remembered for the session: the next dialog that names none starts where the last one was.
      CHECK(dlg.show(mgr, Mode::Open, "Open", kFilters, 3, StringView(), closed.handler()) && dlg.folder().equals("/proj"));
      // A name that does not exist keeps the dialog open, says what is wrong and puts the focus back in the name box.
      w.set_focus(&dlg.name_box());
      type_text(w, "nothere.txt");
      CHECK(dlg.ok_button().enabled());
      w.set_focus(&dlg.ok_button());
      press(w, key::kEnter);
      CHECK(dlg.showing() && w.focus() == &dlg.name_box());
      CHECK(dlg.message_label().text().equals("\"nothere.txt\" could not be found. Check the name, or pick the file from the list."));
      // A single click on a folder and then Open goes into it (no double click needed).
      dlg.name_box().set_text(StringView());
      CHECK(!dlg.ok_button().enabled());
      dlg.list().select_item(0);
      CHECK(dlg.ok_button().enabled());
      click_center(w, dlg.ok_button());
      CHECK(dlg.showing() && dlg.folder().equals("/proj/deep"));
      CHECK(dlg.open_folder("/proj"));
      // Enter presses the default button.
      dlg.name_box().set_text("a.txt");
      w.set_focus(&dlg.name_box());
      press(w, key::kEnter);
      CHECK(!dlg.showing() && dlg.accepted() && dlg.selected_path().equals("/proj/a.txt") && closed.count == 2);

      // Save As offers the current name, selected so that typing replaces it, in the remembered folder.
      CHECK(dlg.show(mgr, Mode::Save, "Save As", kFilters, 3, "Document 1", closed.handler()));
      CHECK(dlg.title().equals("Save As") && dlg.folder().equals("/proj") && dlg.name_box().text().equals("Document 1") && dlg.ok_button().text().equals("Save") && dlg.ok_button().enabled());
      type_text(w, "a");
      CHECK(dlg.name_box().text().equals("a"));
      // Replacing a file is asked about, the question is titled with the file's name, and Cancel is the default: Enter keeps the file.
      press(w, key::kEnter);
      CHECK(dlg.replace_dialog().showing() && dlg.replace_dialog().title().equals("a.txt") && dlg.replace_dialog().default_choice() == MessageDialog::Choice::Decline &&
            dlg.replace_dialog().decline_button().is_default() && dlg.replace_dialog().accept_button().text().equals("Replace"));
      press(w, key::kEnter);
      CHECK(!dlg.replace_dialog().showing() && dlg.showing() && w.focus() == &dlg.name_box());
      press(w, key::kEnter);
      click_center(w, dlg.replace_dialog().accept_button());
      CHECK(!dlg.showing() && dlg.accepted() && dlg.selected_path().equals("/proj/a.txt"));
      // A full stop inside a name is not an extension the user meant: the type's extension is still added. A name that ends in an
      // extension of one of the types offered keeps it.
      CHECK(dlg.show(mgr, Mode::Save, "Save As", kFilters, 3, "/proj/Report v1.2", closed.handler()));
      press(w, key::kEnter);
      CHECK(!dlg.showing() && dlg.selected_path().equals("/proj/Report v1.2.txt"));
      CHECK(dlg.show(mgr, Mode::Save, "Save As", kFilters, 3, "/proj/shot.png", closed.handler()));
      press(w, key::kEnter);
      CHECK(!dlg.showing() && dlg.selected_path().equals("/proj/shot.png"));
      // A folder that does not exist: the dialog stays, with the focus in the name box.
      CHECK(dlg.show(mgr, Mode::Save, "Save As", kFilters, 3, StringView(), closed.handler()));
      dlg.name_box().set_text("/nowhere/x.txt");
      press(w, key::kEnter);
      CHECK(dlg.showing() && w.focus() == &dlg.name_box() && dlg.message_label().text().equals("The folder for \"/nowhere/x.txt\" does not exist. Pick a folder from the list, then type the name."));
      w.render();
      click_center(w, dlg.cancel_button());
      CHECK(!dlg.showing() && !dlg.accepted());
    }
    CHECK(a.live == 0);
  }

  // --- File properties -----------------------------------------------------------------------------------------------------------------------
  {
    TestAllocator a;
    {
      filesystem::RamFileSystem fs(a);
      CHECK(fs.create_directory("/docs") == filesystem::Status::Ok);
      files_make_file(fs, "/docs/a.txt", "hello");
      files_make_file(fs, "/docs/taken.txt", "x");
      Screen screen(a, 800, 600);
      Theme theme;
      Window w(a, screen.renderer, theme, {800, 600});
      WindowManager mgr(a);
      mgr.set_dock(DockStyle::Fill);
      w.add(&mgr);
      desktop::FileProperties props(a);
      props.set_file_system(&fs);
      FilesCounter changed;
      props.on_changed(changed.handler());
      auto open = [&](const char* path) {
        const bool ok = props.show(path);
        if (!props.manager()) CHECK(mgr.add_window(&props));   // modeless: an ordinary frame, not a modal one
        w.layout();
        return ok;
      };
      auto type_name = [&](const char* name) {
        w.set_focus(&props.name_box());
        props.name_box().select_all();
        type_text(w, name);
      };
      // The title is "<Object name> Properties"; OK is the default button, Apply is unavailable while nothing was changed.
      CHECK(open("/docs/a.txt") && props.title().equals("a.txt Properties") && props.value_of("Type").equals("Text Document") && props.name_box().text().equals("a.txt"));
      CHECK(props.ok_button().is_default() && props.ok_button().text().equals("OK") && props.cancel_button().text().equals("Cancel") && props.apply_button().text().equals("Apply") &&
            !props.apply_button().enabled() && !props.pending());
      CHECK(props.ok_button().bounds().width == props.cancel_button().bounds().width && props.cancel_button().bounds().width == props.apply_button().bounds().width);
      CHECK(props.ok_button().bounds().x < props.cancel_button().bounds().x && props.cancel_button().bounds().x < props.apply_button().bounds().x);
      // The label "Name:" comes before its field and its access key leads there.
      props.assign_access_keys();
      w.set_focus(&props.grid());
      alt(w, 'n');
      CHECK(w.focus() == &props.name_box());

      // Apply commits and the sheet stays open, now about the renamed object.
      type_name("b.txt");
      CHECK(props.pending() && props.apply_button().enabled());
      click_center(w, props.apply_button());
      CHECK(props.manager() && files_exists(fs, "/docs/b.txt") && !files_exists(fs, "/docs/a.txt") && props.title().equals("b.txt Properties") && props.path().equals("/docs/b.txt") &&
            changed.count == 1 && !props.apply_button().enabled());
      // Cancel discards what was not applied and closes; it does not undo what Apply did.
      type_name("c.txt");
      click_center(w, props.cancel_button());
      CHECK(!props.manager() && files_exists(fs, "/docs/b.txt") && !files_exists(fs, "/docs/c.txt"));
      // Esc is Cancel.
      CHECK(open("/docs/b.txt"));
      type_name("c.txt");
      press(w, key::kEscape);
      CHECK(!props.manager() && files_exists(fs, "/docs/b.txt") && !files_exists(fs, "/docs/c.txt"));
      // OK (and Enter) applies and closes. A name that cannot be used keeps the sheet open, says why and puts the focus in the field.
      CHECK(open("/docs/b.txt"));
      type_name("taken.txt");
      click_center(w, props.ok_button());
      CHECK(props.manager() && files_exists(fs, "/docs/b.txt") && w.focus() == &props.name_box());
      CHECK(props.message_label().text().equals("\"b.txt\" could not be renamed because that name is already used. Choose another name."));
      type_name("c.txt");
      press(w, key::kEnter);
      CHECK(!props.manager() && files_exists(fs, "/docs/c.txt") && !files_exists(fs, "/docs/b.txt") && changed.count == 2);
      // Close is not Cancel: with a change that was not applied it asks. "Cancel" keeps the sheet, "Don't Apply" drops the change, "Apply" makes it.
      CHECK(open("/docs/c.txt"));
      type_name("d.txt");
      props.close();
      CHECK(props.apply_question().showing() && props.manager() && props.apply_question().title().equals("c.txt Properties") &&
            props.apply_question().accept_button().text().equals("Apply") && props.apply_question().alternate_button().text().equals("Don't Apply"));
      press(w, key::kEscape);
      CHECK(!props.apply_question().showing() && props.manager() && props.pending());
      props.close();
      click_center(w, props.apply_question().alternate_button());
      CHECK(!props.manager() && files_exists(fs, "/docs/c.txt") && !files_exists(fs, "/docs/d.txt"));
      CHECK(open("/docs/c.txt"));
      type_name("d.txt");
      props.close();
      click_center(w, props.apply_question().accept_button());
      CHECK(!props.manager() && files_exists(fs, "/docs/d.txt") && changed.count == 3);
      // With nothing pending the Close button just closes.
      CHECK(open("/docs/d.txt"));
      props.close();
      CHECK(!props.manager() && !props.apply_question().showing());
      // Something that is gone: the sheet says so in a sentence, and there is no name to change.
      CHECK(!open("/docs/gone.txt") && props.title().equals("gone.txt Properties") && !props.name_box().enabled());
      CHECK(props.value_of("Status").equals("\"gone.txt\" cannot be shown because it could not be found. It may have been moved, renamed or deleted."));
      w.render();
      props.close();
    }
    CHECK(a.live == 0);
  }

  // --- Copy progress -------------------------------------------------------------------------------------------------------------------------
  {
    TestAllocator a;
    {
      filesystem::RamFileSystem fs(a);
      CHECK(fs.create_directory("/from") == filesystem::Status::Ok && fs.create_directory("/to") == filesystem::Status::Ok);
      files_make_file(fs, "/from/a.txt", "aaaa");
      files_make_file(fs, "/from/b.txt", "bbbbbb");
      Screen screen(a, 800, 600);
      Theme theme;
      Window w(a, screen.renderer, theme, {800, 600});
      WindowManager mgr(a);
      mgr.set_dock(DockStyle::Fill);
      w.add(&mgr);
      {
        desktop::CopyProgressDialog progress(a);
        // Several items in one transfer; the window says what it is doing and has a Cancel button.
        CHECK(progress.start_batch(mgr, fs, "/from/a.txt\n/from/b.txt", "/to/a.txt\n/to/b.txt", /*remove_source=*/false));
        CHECK(progress.title().equals("Copying") && progress.cancel_button().text().equals("Cancel"));
        CHECK(progress.wait_until_idle(5000000) && w.wait_for_background(5000000));
        CHECK(progress.result() == filesystem::Status::Ok && progress.message().empty() && progress.manager() == nullptr && !progress.can_cancel());
        CHECK(files_exists(fs, "/to/a.txt") && files_exists(fs, "/to/b.txt") && files_exists(fs, "/from/a.txt") && progress.bar().value() == 10 && progress.bar().maximum() == 10);
        CHECK(progress.heading_label().text().equals("Copying file 2 of 2"));
        // Cancel means everything is put back: whenever it lands, an original is never gone without its copy being there, and a
        // cancelled move leaves the originals and no copies.
        CHECK(fs.create_directory("/moved") == filesystem::Status::Ok);
        CHECK(progress.start_batch(mgr, fs, "/from/a.txt\n/from/b.txt", "/moved/a.txt\n/moved/b.txt", /*remove_source=*/true) && progress.title().equals("Moving"));
        progress.close();   // the Close button is Cancel; the window stays until the work has stopped
        CHECK(progress.wait_until_idle(5000000) && w.wait_for_background(5000000) && progress.manager() == nullptr);
        if (progress.result() == desktop::FileTransfer::kCancelled) {
          CHECK(files_exists(fs, "/from/a.txt") && files_exists(fs, "/from/b.txt") && !files_exists(fs, "/moved/a.txt") && !files_exists(fs, "/moved/b.txt") && progress.message().empty());
        } else {
          CHECK(progress.result() == filesystem::Status::Ok && !files_exists(fs, "/from/a.txt") && !files_exists(fs, "/from/b.txt") && files_exists(fs, "/moved/a.txt") && files_exists(fs, "/moved/b.txt"));
        }
        // A copy that cannot be made is reported in one whole message, and nothing half-copied is left.
        CHECK(progress.start_batch(mgr, fs, "/to/a.txt\n/to/missing.txt", "/moved/x.txt\n/moved/y.txt", /*remove_source=*/false));
        CHECK(progress.wait_until_idle(5000000) && w.wait_for_background(5000000));
        CHECK(progress.result() == filesystem::Status::NotFound && !files_exists(fs, "/moved/x.txt"));
        CHECK(progress.message().equals("\"missing.txt\" could not be copied because it could not be found. It may have been moved, renamed or deleted."));
        // Lists of different lengths are refused.
        CHECK(!progress.start_batch(mgr, fs, "/to/a.txt\n/to/b.txt", "/moved/x.txt", false) && !progress.busy());
      }
    }
    CHECK(a.live == 0);
  }
}
