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
// The document applications: the text editor, Paint, the spreadsheet and the database query window.
#include "Desktop/DatabaseQuery.hpp"
#include "Desktop/FileDialog.hpp"
#include "Desktop/Notepad.hpp"
#include "Desktop/Paint.hpp"
#include "Desktop/Spreadsheet.hpp"

// "<Type> <number> - <Application>": the title of a document that has no file yet (7.1).
static bool editors_new_title(StringView title, const char* type, const char* application) {
  const std::string t(title.data(), title.size()), head = std::string(type) + " ", tail = std::string(" - ") + application;
  if (t.size() <= head.size() + tail.size() || t.compare(0, head.size(), head) != 0 || t.compare(t.size() - tail.size(), tail.size(), tail) != 0) return false;
  for (size_t i = head.size(); i < t.size() - tail.size(); ++i)
    if (t[i] < '0' || t[i] > '9') return false;
  return true;
}
static void editors_make_file(filesystem::RamFileSystem& fs, const char* path, const char* text) {
  UniquePointer<filesystem::IFile> file;
  uint64_t written = 0;
  if (fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok)
    file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text), strlen(text)), &written);
}
static bool editors_item_enabled(MenuBar& bar, const char* menu, const char* item) {
  const MenuItem* i = bar.find_item(menu, item);
  return i && i->enabled();
}

static void ui_guide_editors() {
  // --- Text editor ---------------------------------------------------------------------------------------------------------------------------
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    editors_make_file(fs, "/a.txt", "abc");
    kernel::Clipboard cb(a);
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    w.set_clipboard(&cb);
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    NoticeDialog notices(a);
    desktop::FileDialog dlg(a);
    dlg.set_file_system(&fs);
    {
      desktop::Notepad np(a);
      np.set_notices(&notices);
      CHECK(mgr.add_window(&np));
      w.layout();
      MenuBar& bar = np.menu_bar();
      // The title names the document first; a new one is its type and a number. The menus are the standard ones in the standard order.
      CHECK(editors_new_title(np.title(), "Document", "Text Editor"));
      CHECK(standard_menu_order(bar) && menus_have_access_keys(np, bar) && help_menu_ends_in_about(bar, "Text Editor"));
      CHECK(bar.menu_count() == 3 && bar.find_item("File", "Exit") && !bar.find_item("File", "Close"));
      Menu* edit = bar.menu(1);
      CHECK(edit->item(0)->text.view().equals("Undo") && edit->item(1)->text.view().equals("Redo") && bar.find_item("Edit", "Delete")->shortcut.view().equals("Del"));
      // What cannot work is disabled, not answered with a message: nothing to undo or cut, no file dialog, nothing to print.
      bar.open_menu(1);
      w.close_popup();
      CHECK(!editors_item_enabled(bar, "Edit", "Undo") && !editors_item_enabled(bar, "Edit", "Redo") && !editors_item_enabled(bar, "Edit", "Cut") &&
            !editors_item_enabled(bar, "Edit", "Delete") && !editors_item_enabled(bar, "Edit", "Paste"));
      CHECK(!choose(w, bar, "File", "Open...") && !choose(w, bar, "File", "Save As...") && !choose(w, bar, "File", "Print...") && !notices.showing());
      press(w, key::kA + 14, modifier::kLeftControl);   // Ctrl+O with nothing to open with: nothing happens
      CHECK(!notices.showing() && !dlg.showing());
      np.set_file_system(&fs);
      np.set_file_dialog(&dlg);
      // Undo and Redo from the menu; Delete removes the selection.
      w.set_focus(&np.editor());
      type_text(w, "hello");
      CHECK(choose(w, bar, "Edit", "Undo") && np.editor().text().empty() && choose(w, bar, "Edit", "Redo") && np.editor().text().equals("hello"));
      CHECK(choose(w, bar, "Edit", "Select All") && choose(w, bar, "Edit", "Delete") && np.editor().text().empty());
      type_text(w, "note");
      // Alt and the access key of a title opens the menu from the keyboard.
      alt(w, 'f');
      CHECK(bar.open_menu_index() == 0);
      press(w, key::kEscape);
      CHECK(bar.open_menu_index() == -1);
      // New with unsaved changes asks Save / Don't Save / Cancel. Esc (Cancel) leaves everything as it was.
      gui::MessageDialog& ask = np.save_question();
      CHECK(choose(w, bar, "File", "New") && ask.showing() && np.editor().text().equals("note") && ask.title().equals(np.document_name()));
      CHECK(ask.accept_button().text().equals("Save") && ask.alternate_button().text().equals("Don't Save") && ask.decline_button().text().equals("Cancel"));
      press(w, key::kEscape);
      CHECK(!ask.showing() && np.editor().text().equals("note") && np.editor().modified());
      // Don't Save: the new document starts, with the next number.
      String before(a);
      before.assign(np.title());
      press(w, key::kA + 13, modifier::kLeftControl);   // Ctrl+N
      CHECK(ask.showing());
      ask.alternate_button().click();
      CHECK(!ask.showing() && np.editor().text().empty() && !np.editor().modified() && editors_new_title(np.title(), "Document", "Text Editor") && !np.title().equals(before.view()));
      // Open with unsaved changes: Save needs a name first (Save As offers the document's name), and the Open dialog follows the save.
      type_text(w, "keep");
      CHECK(choose(w, bar, "File", "Open...") && ask.showing() && !dlg.showing());
      press(w, key::kEnter);   // the default button: Save
      CHECK(!ask.showing() && dlg.showing() && dlg.mode() == desktop::FileDialog::Mode::Save && dlg.title().equals("Save As"));
      CHECK(dlg.name_box().text().size() > 13 && dlg.name_box().text().first(9).equals("Document ") && dlg.name_box().text().substr(dlg.name_box().text().size() - 4).equals(".txt"));
      dlg.name_box().set_text("kept.txt");
      dlg.ok_button().click();
      CHECK(np.title().equals("kept.txt - Text Editor") && np.status_label().text().equals("Saved \"kept.txt\".") && !np.editor().modified());
      CHECK(dlg.showing() && dlg.mode() == desktop::FileDialog::Mode::Open && dlg.title().equals("Open"));
      dlg.name_box().set_text("a.txt");
      dlg.ok_button().click();
      CHECK(!dlg.showing() && np.editor().text().equals("abc") && np.title().equals("a.txt - Text Editor") && np.status_label().text().equals("Opened \"a.txt\"."));
      // A Save As that is cancelled drops what waited for it: the document stays.
      type_text(w, "x");
      CHECK(np.new_document() == false && ask.showing());
      ask.alternate_button().click();
      type_text(w, "y");
      CHECK(choose(w, bar, "File", "New") && ask.showing());
      press(w, key::kEnter);
      CHECK(dlg.showing());
      dlg.cancel_button().click();
      CHECK(!dlg.showing() && np.editor().text().equals("y") && np.editor().modified());
      CHECK(choose(w, bar, "File", "Save As...") && dlg.showing());   // an ordinary save afterwards does not start a new document
      dlg.name_box().set_text("y.txt");
      dlg.ok_button().click();
      CHECK(np.editor().text().equals("y") && np.title().equals("y.txt - Text Editor"));
      // Messages are whole sentences that name the document and say what to do.
      CHECK(!np.open_file("/gone.txt") && notices.showing() && notices.kind() == NoticeKind::Error &&
            notices.message().equals("\"gone.txt\" could not be opened. It is no longer there. It may have been moved, renamed or deleted."));
      notices.ok_button().click();
      CHECK(!np.save_file("/missing/x.txt") && notices.showing() && notices.message().first(33).equals("\"x.txt\" could not be saved. The f") && np.path().equals("/y.txt"));
      notices.ok_button().click();
      // Help > About.
      CHECK(choose(w, bar, "Help", "About Text Editor") && notices.showing() && notices.kind() == NoticeKind::Information && notices.title().equals("About Text Editor"));
      notices.ok_button().click();
      // Closing the frame with unsaved changes asks too; Cancel keeps it open, Don't Save closes it and the changes are gone.
      type_text(w, "z");
      np.close();
      CHECK(np.manager() == &mgr && ask.showing());
      press(w, key::kEscape);
      CHECK(np.manager() == &mgr && !ask.showing());
      CHECK(choose(w, bar, "File", "Exit") && ask.showing() && np.manager() == &mgr);
      ask.alternate_button().click();
      CHECK(np.manager() == nullptr && np.editor().text().empty() && !np.editor().modified());
      // Save on closing: the file is written, then the frame closes.
      CHECK(mgr.add_window(&np));
      w.layout();
      w.set_focus(&np.editor());
      CHECK(np.open_file("/a.txt"));
      type_text(w, "!");
      np.close();
      CHECK(ask.showing());
      ask.accept_button().click();
      CHECK(np.manager() == nullptr && !np.editor().modified());
      UniquePointer<filesystem::IFile> file;
      CHECK(fs.open_file("/a.txt", filesystem::OpenMode::Read, &file) == filesystem::Status::Ok && file->size() == 4);
      file.reset();
      // Nothing to lose: it just closes.
      CHECK(mgr.add_window(&np));
      np.close();
      CHECK(np.manager() == nullptr && !ask.showing());
    }

    // --- Paint ---------------------------------------------------------------------------------------------------------------------------------
    {
      desktop::Paint paint(a);
      paint.set_notices(&notices);
      paint.set_file_system(&fs);
      paint.set_file_dialog(&dlg);
      CHECK(mgr.add_window(&paint));
      w.layout();
      MenuBar& bar = paint.menu_bar();
      DrawingCanvas& c = paint.canvas();
      CHECK(editors_new_title(paint.title(), "Picture", "Paint"));
      CHECK(standard_menu_order(bar) && menus_have_access_keys(paint, bar) && help_menu_ends_in_about(bar, "Paint"));
      // No "Options" menu: the drawing tools and their switch are in Tools, the colors in Colors.
      CHECK(!bar.find_item("Options", "Filled Shapes") && bar.find_item("Tools", "Filled Shapes") && bar.find_item("Colors", "Primary Color...") && bar.find_item("File", "Exit"));
      CHECK(bar.find_item("Edit", "Delete") && paint.tools().tool_name(6).equals("Color Picker"));
      // Every tool of the strip is a menu command too (nothing lives only on a toolbar); the tool in use has the check mark.
      CHECK(choose(w, bar, "Tools", "Rectangle") && c.tool() == PaintTool::Rectangle && paint.tools().selected_tool() == 3);
      bar.open_menu(3);
      w.close_popup();
      CHECK(bar.find_item("Tools", "Rectangle")->checked() && !bar.find_item("Tools", "Pencil")->checked());
      CHECK(choose(w, bar, "Tools", "Pencil") && c.tool() == PaintTool::Pencil);
      // Disabled, not left to fail: no selection to cut or delete, no picture to paste, no print or color dialog, nothing to zoom out of.
      CHECK(!choose(w, bar, "Edit", "Cut") && !choose(w, bar, "Edit", "Delete") && !choose(w, bar, "Edit", "Paste") && !choose(w, bar, "Edit", "Undo"));
      CHECK(!choose(w, bar, "File", "Print...") && !choose(w, bar, "Colors", "Primary Color...") && !choose(w, bar, "View", "Zoom Out") && !choose(w, bar, "View", "Actual Size"));
      CHECK(!notices.showing());
      CHECK(choose(w, bar, "View", "Zoom In") && c.zoom() == 2 && choose(w, bar, "View", "Actual Size") && c.zoom() == 1);
      press(w, key::kA + 23, modifier::kLeftControl);   // Ctrl+X with nothing selected: nothing happens, no message
      CHECK(!notices.showing() && !c.modified());
      // The label of the brush size ends in a colon, so Alt and its letter moves the focus to the slider.
      alt(w, 's');
      CHECK(w.focus() == &paint.size_slider());
      // A stroke, then New: the question; Cancel keeps the picture, Don't Save starts the next one.
      const Point at = c.image_to_view(20, 20);
      const Point origin = c.window_origin();
      click(w, origin.x + at.x, origin.y + at.y);
      CHECK(c.modified() && c.pixel(20, 20) == 0xFF000000);
      gui::MessageDialog& ask = paint.save_question();
      CHECK(choose(w, bar, "File", "New") && ask.showing() && c.pixel(20, 20) == 0xFF000000);
      press(w, key::kEscape);
      CHECK(!ask.showing() && c.modified());
      CHECK(choose(w, bar, "Edit", "Undo") && c.pixel(20, 20) == 0xFFFFFFFF && choose(w, bar, "Edit", "Redo") && c.pixel(20, 20) == 0xFF000000);
      paint.close();
      CHECK(paint.manager() == &mgr && ask.showing());
      press(w, key::kEnter);   // Save: Save As offers "Picture N.png"
      CHECK(dlg.showing() && dlg.name_box().text().first(8).equals("Picture ") && dlg.name_box().text().substr(dlg.name_box().text().size() - 4).equals(".png"));
      dlg.name_box().set_text("dot.png");
      dlg.ok_button().click();
      CHECK(paint.manager() == nullptr && paint.title().equals("dot.png - Paint") && !c.modified());
      // A file that is not a picture: the message names it and says why.
      CHECK(mgr.add_window(&paint));
      CHECK(!paint.open_file("/a.txt") && notices.showing() && notices.kind() == NoticeKind::Error &&
            notices.message().equals("\"a.txt\" could not be opened. It is not a picture of a kind Paint can read, or the file is damaged."));
      notices.ok_button().click();
      paint.close();
    }

    // --- Spreadsheet ---------------------------------------------------------------------------------------------------------------------------
    {
      desktop::Spreadsheet sp(a);
      sp.set_notices(&notices);
      sp.set_file_system(&fs);
      sp.set_file_dialog(&dlg);
      CHECK(mgr.add_window(&sp));
      w.layout();
      MenuBar& bar = sp.menu_bar();
      CHECK(editors_new_title(sp.title(), "Sheet", "Spreadsheet"));
      CHECK(standard_menu_order(bar) && menus_have_access_keys(sp, bar) && help_menu_ends_in_about(bar, "Spreadsheet") && bar.find_item("File", "Exit"));
      CHECK(!choose(w, bar, "Edit", "Undo") && !choose(w, bar, "Edit", "Redo"));
      // An entry, then Undo and Redo from the keyboard and from the menu.
      w.set_focus(&sp.grid());
      type_text(w, "5");
      press(w, key::kEnter);
      CHECK(sp.sheet().text(0, 0).equals("5") && sp.can_undo() && !sp.can_redo());
      ctrl(w, 'z');
      CHECK(sp.sheet().text(0, 0).empty() && sp.can_redo() && sp.grid().active_row() == 0);
      ctrl(w, 'y');
      CHECK(sp.sheet().text(0, 0).equals("5"));
      CHECK(choose(w, bar, "Edit", "Undo") && sp.sheet().text(0, 0).empty() && choose(w, bar, "Edit", "Redo") && sp.sheet().text(0, 0).equals("5"));
      // Delete is not final any more: it can be taken back.
      sp.grid().select(0, 0);
      CHECK(choose(w, bar, "Edit", "Delete") && sp.sheet().text(0, 0).empty() && choose(w, bar, "Edit", "Undo") && sp.sheet().text(0, 0).equals("5"));
      // The cells have a context menu; a right click on a cell outside the selection selects it first. Its commands are those of the Edit menu.
      const Rect2D cell = sp.grid().cell_rect(1, 2);
      const Point origin = sp.grid().window_origin();
      right_click(w, origin.x + cell.x + 5, origin.y + cell.y + 5);
      CHECK(sp.cell_menu().open() && sp.grid().active_column() == 1 && sp.grid().active_row() == 2);
      CHECK(sp.cell_menu().find_item("Cut") && sp.cell_menu().find_item("Paste") && sp.cell_menu().find_item("Delete") && sp.cell_menu().find_item("Select All"));
      CHECK(!sp.cell_menu().find_item("Paste")->enabled());   // nothing on the clipboard
      w.close_popup();
      sp.grid().select(0, 0);
      CHECK(choose(w, bar, "Edit", "Copy") && cb.has_text());
      sp.grid().select(1, 2);
      CHECK(choose(w, bar, "Edit", "Paste") && sp.sheet().text(1, 2).equals("5") && choose(w, bar, "Edit", "Undo") && sp.sheet().text(1, 2).empty());
      CHECK(sp.summary_label().text().empty());
      sp.grid().select(0, 0);
      sp.grid().select(0, 1, true);
      CHECK(sp.summary_label().text().equals("Count: 1   Sum: 5   Average: 5"));
      // Unsaved changes: New asks; Cancel keeps the sheet; closing asks; Don't Save closes.
      gui::MessageDialog& ask = sp.save_question();
      CHECK(choose(w, bar, "File", "New") && ask.showing());
      press(w, key::kEscape);
      CHECK(sp.sheet().text(0, 0).equals("5"));
      sp.close();
      CHECK(sp.manager() == &mgr && ask.showing());
      ask.alternate_button().click();
      CHECK(sp.manager() == nullptr && sp.sheet().text(0, 0).empty() && !sp.can_undo());
    }

    // --- Database query ------------------------------------------------------------------------------------------------------------------------
    {
      desktop::DatabaseQuery q(a);
      q.set_notices(&notices);
      q.set_file_system(&fs);
      q.set_file_dialog(&dlg);
      CHECK(mgr.add_window(&q));
      w.layout();
      MenuBar& bar = q.menu_bar();
      CHECK(editors_new_title(q.title(), "Query", "Database"));
      CHECK(standard_menu_order(bar) && menus_have_access_keys(q, bar) && help_menu_ends_in_about(bar, "Database") && bar.find_item("File", "Exit"));
      CHECK(bar.find_item("Query", "Run")->shortcut.view().equals("F5") && bar.find_item("Edit", "Undo") && bar.find_item("File", "Export Results..."));
      // Nothing to connect to, no connection, no table: the commands are disabled.
      CHECK(!q.connect_button().enabled() && !choose(w, bar, "Query", "Connect") && !choose(w, bar, "Query", "Run") && !choose(w, bar, "Query", "Disconnect"));
      CHECK(!choose(w, bar, "File", "Export Results...") && !choose(w, bar, "Edit", "Copy Results") && !notices.showing());
      // The connection bar reads server, password, Connect, and Tab goes the same way; the labels' access keys reach the boxes.
      CHECK(q.url_box().window_origin().x < q.password_box().window_origin().x && q.password_box().window_origin().x < q.connect_button().window_origin().x);
      alt(w, 's');
      CHECK(w.focus() == &q.url_box());
      press(w, key::kTab);
      CHECK(w.focus() == &q.password_box());
      alt(w, 's');
      alt(w, 'p');
      CHECK(w.focus() == &q.password_box());
      // F5 without a connection says what to do, as information.
      press(w, key::kF1 + 4);
      CHECK(notices.showing() && notices.kind() == NoticeKind::Information &&
            notices.message().equals("No database is connected, so the statement was not run. Enter the server, choose Connect, and run it again."));
      notices.ok_button().click();
      // The table of results has a menu with its two commands.
      CHECK(q.grid().context_menu() == &q.results_menu() && q.results_menu().find_item("Copy Results") && q.results_menu().find_item("Export Results..."));
      // The SQL is a document like any other: Undo in the menu, the question before it is lost.
      w.set_focus(&q.editor());
      type_text(w, "SELECT 1");
      CHECK(choose(w, bar, "Edit", "Undo") && q.editor().text().empty() && choose(w, bar, "Edit", "Redo") && q.editor().text().equals("SELECT 1"));
      gui::MessageDialog& ask = q.save_question();
      CHECK(!q.open_file("/a.txt") && ask.showing() && q.editor().text().equals("SELECT 1"));
      ask.alternate_button().click();   // Don't Save: the file that was asked for opens
      CHECK(q.editor().text().equals("!abc") && q.title().equals("a.txt - Database") && q.status_label().text().equals("Opened \"a.txt\"."));
      w.set_focus(&q.editor());
      type_text(w, "d");
      q.close();
      CHECK(q.manager() == &mgr && ask.showing());
      press(w, key::kEscape);
      CHECK(q.manager() == &mgr);
      CHECK(choose(w, bar, "File", "Exit") && ask.showing());
      ask.alternate_button().click();
      CHECK(q.manager() == nullptr);
    }
  }
  CHECK(a.live == 0);
}
