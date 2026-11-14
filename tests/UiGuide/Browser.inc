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
// The web browser's frame against the interface guidelines: the title, the menus and what is disabled in them, the toolbar, the keys, the context
// menu of the page, Stop, saving a page, and the wording of its messages.
#include <functional>
#include <map>
#include "Desktop/Desktop.hpp"
#include "Desktop/FileDialog.hpp"
#include "Desktop/WebBrowser.hpp"
#include "Gui/Windows/TabFrame.hpp"

namespace {

// Hands out canned pages. `during` runs while a page is being fetched (the place to press Stop).
struct BrowserGuideLoader final : browser::IPageLoader {
  std::map<std::string, std::string> pages;   // address -> html
  std::function<void()> during;
  int asked = 0;
  bool fetch(StringView address, browser::PageResult* r) override {
    ++asked;
    if (during) during();
    const std::string key(address.data(), address.size());
    if (key == "http://down.test/") {
      r->error.assign("the server refused the connection.");
      return false;
    }
    r->status = 200;
    r->final_address.assign(address);
    std::string html = "<html><head><title>Plain</title></head><body><p>Page <a href=\"/next\">next</a> <a href=\"mailto:someone@example.com\">write</a></p></body></html>";
    r->content_type.assign("text/html");
    if (key == "http://one.test/next") html = "<title>Two</title><h1>Two</h1>";
    if (key == "http://three.test/") html = "<title>Three</title><p>three</p>";
    if (key == "http://cookie.test/") html = "<title>Cookie</title><script>document.cookie = 'a=1; max-age=1000'</script><p>cookie</p>";
    if (key == "http://leave.test/") html = "<title>Leave</title><script>addEventListener('beforeunload', function (e) { e.preventDefault() })</script><p>stay</p>";
    if (key == "http://field.test/") html = "<title>Field</title><input id=f value=abc><script>document.getElementById('f').focus()</script>";
    if (key == "http://data.test/") {
      r->content_type.assign("image/png");
      html = std::string("\x89PNG\0\0\0\0binary", 15);
    }
    r->body.append(ConstByteView(reinterpret_cast<const uint8_t*>(html.data()), html.size()));
    return true;
  }
};

const paint::Op* browser_text_op(desktop::WebBrowser& b, const char* text) {
  const browser::PagePicture* picture = b.page().picture();
  for (uint64_t i = 0; picture && i < picture->list.count(); ++i) {
    const paint::Op& op = picture->list.op(i);
    if (op.kind == paint::OpKind::Text && picture->list.text(op).contains(text)) return &op;
  }
  return nullptr;
}
bool browser_text_point(desktop::WebBrowser& b, const char* text, int32_t* x, int32_t* y) {
  const paint::Op* op = browser_text_op(b, text);
  if (!op) return false;
  const Point o = b.page().window_origin();
  *x = o.x + op->rect.x + op->rect.width / 2;
  *y = o.y + op->rect.y + op->rect.height / 2 - b.page().scroll_y();
  return true;
}
void browser_make_file(filesystem::IFileSystem& fs, const char* path, const char* text) {
  UniquePointer<filesystem::IFile> file;
  uint64_t written = 0;
  CHECK(fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok);
  CHECK(file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text), strlen(text)), &written) == filesystem::Status::Ok && written == strlen(text));
}
std::string browser_read_file(filesystem::IFileSystem& fs, const char* path) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(path, filesystem::OpenMode::Read, &file) != filesystem::Status::Ok) return "<missing>";
  std::string text(file->size(), '\0');
  uint64_t got = 0;
  if (!text.empty()) file->read(0, ArrayView<uint8_t>(reinterpret_cast<uint8_t*>(text.data()), text.size()), &got);
  return text.substr(0, got);
}
bool item_enabled(MenuBar& bar, const char* menu, const char* item) {
  const MenuItem* i = bar.find_item(menu, item);
  return i && i->enabled();
}
bool shortcut_is(MenuBar& bar, const char* menu, const char* item, const char* shortcut) {
  const MenuItem* i = bar.find_item(menu, item);
  return i && i->shortcut.view().equals(shortcut);
}
// Opens a menu the way F10 / a click does, so the browser brings its items up to date, and closes it again.
void refresh_menus(Window& w, MenuBar& bar) {
  bar.open_menu(0);
  if (w.popup()) w.close_popup();
}

}  // namespace

static void ui_guide_browser() {
  // --- The frame on its own: title, menus, toolbar, keys, messages.
  TestAllocator a;
  {
    filesystem::RamFileSystem fs(a);
    CHECK(fs.create_directory("/Desktop") == filesystem::Status::Ok && fs.create_directory(filesystem::layout::kDownloads) == filesystem::Status::Ok);
    browser_make_file(fs, "/notes.txt", "plain text");
    Screen screen(a, 900, 650);
    Theme theme;
    Window w(a, screen.renderer, theme, {900, 650});
    kernel::Clipboard clipboard(a);
    w.set_clipboard(&clipboard);
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    NoticeDialog notices(a);
    desktop::FileDialog files(a);
    files.set_file_system(&fs);
    desktop::WebBrowser b(a);
    BrowserGuideLoader loader;
    CHECK(mgr.add_window(&b));
    b.set_file_system(&fs);
    b.set_page_loader(&loader);
    b.set_notices(&notices);
    b.set_file_dialog(&files);
    w.layout();
    MenuBar& bar = b.menu_bar();

    // The title: the page first, then the program; a new tab has a name of its own; a file is named as the explorer names it (no path).
    CHECK(b.title().equals("New Tab - Web Browser"));
    CHECK(browser_text_op(b, "To open a page") && browser_text_op(b, "Press Enter."));   // the start page says how, in steps

    // The menus: the standard order, access keys, Help ends in About, File ends in a separator and Close.
    CHECK(bar.menu_count() == 6 && standard_menu_order(bar) && menus_have_access_keys(b, bar) && help_menu_ends_in_about(bar, "Web Browser"));
    {
      Menu* file = bar.menu(0);
      CHECK(file->title().equals("File") && file->item(file->item_count() - 1)->text.view().equals("Close") && file->item(file->item_count() - 2)->separator);
      Menu* edit = bar.menu(1);
      CHECK(edit->title().equals("Edit") && edit->item(0)->text.view().equals("Cut") && edit->item(1)->text.view().equals("Copy") && edit->item(2)->text.view().equals("Paste") &&
            edit->item(3)->text.view().equals("Delete") && edit->item(5)->text.view().equals("Select All"));
    }
    // An ellipsis only where more is asked before the command runs; shortcuts written as on the key caps.
    CHECK(bar.find_item("File", "Open Location...") && bar.find_item("File", "Open File...") && bar.find_item("File", "Save Page As...") && bar.find_item("File", "New Tab"));
    CHECK(bar.find_item("Tools", "Clear Cookies and Site Data") && bar.find_item("Help", "Help Topics") && bar.find_item("Help", "About Web Browser"));
    CHECK(shortcut_is(bar, "File", "New Tab", "Ctrl+T") && shortcut_is(bar, "File", "Open Location...", "Ctrl+L") && shortcut_is(bar, "File", "Open File...", "Ctrl+O") &&
          shortcut_is(bar, "File", "Save Page As...", "Ctrl+S") && shortcut_is(bar, "File", "Close", "Ctrl+W"));
    CHECK(shortcut_is(bar, "Go", "Back", "Alt+Left") && shortcut_is(bar, "Go", "Forward", "Alt+Right") && shortcut_is(bar, "Go", "Reload", "F5") && shortcut_is(bar, "Go", "Stop", "Esc") &&
          shortcut_is(bar, "Edit", "Delete", "Del") && shortcut_is(bar, "Help", "Help Topics", "F1"));

    // What does not apply is disabled, in the menus and on the toolbar: nowhere to go back to, nothing loading, nothing to reload or save, no
    // tabs to add to (the frame is not in a tab window here), and no text to cut while the focus is not in a text field.
    refresh_menus(w, bar);
    CHECK(!b.back_button().enabled() && !b.forward_button().enabled() && !b.reload_button().enabled() && !b.stop_button().enabled() && b.go_button().enabled());
    CHECK(!item_enabled(bar, "Go", "Back") && !item_enabled(bar, "Go", "Forward") && !item_enabled(bar, "Go", "Reload") && !item_enabled(bar, "Go", "Stop"));
    CHECK(!item_enabled(bar, "File", "Save Page As...") && !item_enabled(bar, "File", "New Tab") && item_enabled(bar, "File", "Open File...") && item_enabled(bar, "File", "Close"));
    w.set_focus(&b.go_button());
    refresh_menus(w, bar);
    CHECK(!item_enabled(bar, "Edit", "Cut") && !item_enabled(bar, "Edit", "Copy") && !item_enabled(bar, "Edit", "Paste") && !item_enabled(bar, "Edit", "Delete") && !item_enabled(bar, "Edit", "Select All"));
    CHECK(!choose(w, bar, "Go", "Back") && b.address().empty());

    // Every toolbar button has a text label (so an access key, and no need of a tooltip), and its command is in a menu too.
    CHECK(b.back_button().text().equals("Back") && b.forward_button().text().equals("Forward") && b.reload_button().text().equals("Reload") && b.stop_button().text().equals("Stop") &&
          b.go_button().text().equals("Go") && b.address_label().text().equals("Address:"));
    CHECK(bar.find_item("Go", "Back") && bar.find_item("Go", "Forward") && bar.find_item("Go", "Reload") && bar.find_item("Go", "Stop") && bar.find_item("File", "Open Location..."));

    // Keyboard only: Alt+A (the label "Address:") goes to the address box, typing and Enter open the page; Tab goes on to Go, in reading order.
    alt(w, 'a');
    CHECK(b.address_box().focused());
    type_text(w, "one.test");
    press(w, key::kEnter);
    CHECK(b.address().equals("http://one.test/") && b.title().equals("Plain - Web Browser") && b.status_label().text().equals("Done."));
    w.set_focus(&b.address_box());
    press(w, key::kTab);
    CHECK(b.go_button().focused());
    press(w, key::kTab, modifier::kLeftShift);
    CHECK(b.address_box().focused());
    // Ctrl+L goes there from anywhere and selects the address; F6 goes to the page and back.
    w.set_focus(&b.page());
    ctrl(w, 'l');
    CHECK(b.address_box().focused() && b.address_box().has_selection() && b.address_box().selected_text().equals("http://one.test/"));
    press(w, key::kF1 + 5);
    CHECK(b.page().focused());
    press(w, key::kF1 + 5);
    CHECK(b.address_box().focused());
    CHECK(choose(w, bar, "File", "Open Location...") && b.address_box().focused());

    // Edit acts on the address box: Copy, Cut, Paste, Select All, Delete, each enabled only when it applies.
    b.address_box().select_all();
    refresh_menus(w, bar);
    CHECK(item_enabled(bar, "Edit", "Cut") && item_enabled(bar, "Edit", "Copy") && item_enabled(bar, "Edit", "Paste") && item_enabled(bar, "Edit", "Delete") && item_enabled(bar, "Edit", "Select All"));
    CHECK(choose(w, bar, "Edit", "Copy"));
    {
      String copied(a);
      CHECK(clipboard.get_text(&copied) && copied.view().equals("http://one.test/"));
    }
    CHECK(choose(w, bar, "Edit", "Cut") && b.address_box().text().empty());
    CHECK(!choose(w, bar, "Edit", "Cut") && !item_enabled(bar, "Edit", "Select All") && item_enabled(bar, "Edit", "Paste"));   // nothing selected, nothing there
    CHECK(choose(w, bar, "Edit", "Paste") && b.address_box().text().equals("http://one.test/"));
    CHECK(choose(w, bar, "Edit", "Select All") && b.address_box().has_selection());
    CHECK(choose(w, bar, "Edit", "Delete") && b.address_box().text().empty());
    // Esc in the address box puts the address of the page back.
    type_text(w, "half typed");
    press(w, key::kEscape);
    CHECK(b.address_box().text().equals("http://one.test/") && b.address().equals("http://one.test/"));

    // Back and Forward: Alt+Left / Alt+Right also while the focus is in the address box, the buttons' access keys, the menu items.
    CHECK(b.navigate("http://one.test/next") && b.title().equals("Two - Web Browser") && b.back_button().enabled() && b.reload_button().enabled());
    w.set_focus(&b.address_box());
    press(w, key::kLeft, modifier::kLeftAlt);
    CHECK(b.address().equals("http://one.test/") && b.forward_button().enabled() && !b.back_button().enabled());
    press(w, key::kRight, modifier::kLeftAlt);
    CHECK(b.address().equals("http://one.test/next"));
    alt(w, 'b');
    CHECK(b.address().equals("http://one.test/"));
    alt(w, 'w');
    CHECK(b.address().equals("http://one.test/next"));
    CHECK(choose(w, bar, "Go", "Back") && b.address().equals("http://one.test/") && !choose(w, bar, "Go", "Back"));
    CHECK(choose(w, bar, "Go", "Forward") && b.address().equals("http://one.test/next"));
    {
      const int before = loader.asked;
      CHECK(choose(w, bar, "Go", "Reload") && loader.asked == before + 1);
      alt(w, 'r');
      CHECK(loader.asked == before + 2 && b.address().equals("http://one.test/next"));
    }

    // The pages Back and Forward lead to are in the Go menu as well (not only in the buttons' context menus); the page shown has the check mark.
    CHECK(b.navigate("http://three.test/"));
    refresh_menus(w, bar);
    {
      const MenuItem* shown = bar.find_item("Go", "Three");
      CHECK(shown && shown->checked() && bar.find_item("Go", "Two") && bar.find_item("Go", "Plain") && !bar.find_item("Go", "Plain")->checked());
      CHECK(choose(w, bar, "Go", "Plain") && b.address().equals("http://one.test/") && b.forward_button().enabled());
      CHECK(choose(w, bar, "Go", "Three") && b.address().equals("http://three.test/"));
    }

    // Stop: enabled only while a page is being fetched; what comes back is dropped and the page that was shown stays. Esc does the same.
    {
      bool stop_was_enabled = false, item_was_enabled = false;
      loader.during = [&] {
        stop_was_enabled = b.stop_button().enabled() && b.loading();
        refresh_menus(w, bar);
        item_was_enabled = item_enabled(bar, "Go", "Stop");
        click_center(w, b.stop_button());
      };
      CHECK(b.navigate("http://one.test/"));
      CHECK(stop_was_enabled && item_was_enabled && !b.loading() && !b.stop_button().enabled());
      CHECK(b.address().equals("http://three.test/") && b.address_box().text().equals("http://three.test/") && b.title().equals("Three - Web Browser") && b.status_label().text().equals("Stopped."));
      loader.during = [&] { press(w, key::kEscape); };
      w.set_focus(&b.go_button());
      CHECK(b.navigate("http://one.test/") && b.address().equals("http://three.test/") && b.status_label().text().equals("Stopped."));
      loader.during = nullptr;
      CHECK(b.navigate("http://one.test/") && b.address().equals("http://one.test/"));   // and the next page loads as usual
    }

    // The page's context menu: "Open Link" on a link (disabled elsewhere), then the page's commands, all of them in the menu bar too.
    {
      int32_t x = 0, y = 0;
      CHECK(browser_text_point(b, "next", &x, &y));
      right_click(w, x, y);
      CHECK(b.page_menu().open() && b.page_menu().find_item("Open Link") && b.page_menu().find_item("Open Link")->enabled());
      CHECK(b.page_menu().find_item("Back") && b.page_menu().find_item("Forward") && b.page_menu().find_item("Reload") && b.page_menu().find_item("Save Page As..."));
      CHECK(click_popup_item(w, &b.page_menu().menu(), "Open Link") && b.address().equals("http://one.test/next"));
      const Point origin = b.page().window_origin();
      right_click(w, origin.x + 400, origin.y + 300);
      CHECK(b.page_menu().open() && !b.page_menu().find_item("Open Link")->enabled() && b.page_menu().find_item("Back")->enabled() && !b.page_menu().find_item("Forward")->enabled());
      CHECK(click_popup_item(w, &b.page_menu().menu(), "Back") && b.address().equals("http://one.test/"));
      // From the keyboard: Shift+F10 with the focus on the page.
      w.mouse_move(origin.x + 400, origin.y + 300);
      w.set_focus(&b.page());
      press(w, key::kF1 + 9, modifier::kLeftShift);
      CHECK(b.page_menu().open());
      press(w, key::kEscape);
      CHECK(!b.page_menu().open());
    }

    // Messages: what happened, why, what to do; named by the page; no codes; never "Error" as the title.
    {
      int32_t x = 0, y = 0;
      CHECK(browser_text_point(b, "write", &x, &y));
      right_click(w, x, y);   // "Open Link" on a link this browser cannot follow says so (not only in the status bar)
      CHECK(click_popup_item(w, &b.page_menu().menu(), "Open Link"));
      CHECK(notices.showing() && notices.kind() == NoticeKind::Warning && notices.title().equals("Plain - Web Browser"));
      CHECK(notices.message().equals("\"mailto:someone@example.com\" cannot be opened here. This browser opens web addresses (http and https) and files on this computer."));
      press(w, key::kEnter);
      CHECK(!notices.showing() && b.address().equals("http://one.test/"));

      CHECK(b.navigate("http://down.test/") && notices.showing() && notices.kind() == NoticeKind::Error);
      CHECK(notices.message().equals("\"down.test\" could not be opened because the server refused the connection. Check the address and the network connection, then try again."));
      CHECK(!notices.message().contains("rror") && !notices.title().contains("rror") && b.address().equals("http://one.test/") && b.address_box().text().equals("http://one.test/"));
      press(w, key::kEnter);

      CHECK(b.navigate("/Desktop/gone.html") && notices.showing() && notices.kind() == NoticeKind::Error);
      CHECK(notices.message().first(35).equals("\"gone.html\" could not be opened. It") && !notices.message().contains("/Desktop"));
      press(w, key::kEnter);

      b.set_page_loader(nullptr);
      CHECK(b.navigate("http://one.test/next") && notices.showing());
      CHECK(notices.message().equals("\"next\" could not be opened because this computer is not connected to a network. Check the connection, then try again."));
      press(w, key::kEnter);
      b.set_page_loader(&loader);
      CHECK(!notices.showing());
    }

    // A file the browser cannot show says what to do with it, and Save Page As keeps it: the Downloads folder is offered, with the page's name.
    CHECK(b.navigate("http://data.test/") && browser_text_op(b, "cannot be shown here (image/png)") && browser_text_op(b, "choose Save Page As on the File menu"));
    refresh_menus(w, bar);
    CHECK(b.can_save() && item_enabled(bar, "File", "Save Page As..."));
    CHECK(choose(w, bar, "File", "Save Page As...") && files.showing() && files.title().equals("Save Page As") && files.mode() == desktop::FileDialog::Mode::Save);
    CHECK(files.folder().equals(filesystem::layout::kDownloads) && files.name_box().text().equals("data.test"));
    files.accept_name();
    CHECK(!files.showing() && browser_read_file(fs, "/Desktop/Downloads/data.test") == std::string("\x89PNG\0\0\0\0binary", 15) && b.status_label().text().equals("Saved \"data.test\"."));
    // Saving over a file that is there is asked about first, and the button that keeps the file is the safe one.
    w.set_focus(&b.page());
    ctrl(w, 's');
    CHECK(files.showing());
    files.accept_name();
    CHECK(files.showing() && files.replace_dialog().showing());
    press(w, key::kEscape);
    CHECK(!files.replace_dialog().showing());
    if (files.showing()) files.close();
    // A page is offered as a web page, and a text file keeps its name.
    CHECK(b.navigate("http://one.test/"));
    b.ask_save();
    CHECK(files.showing() && files.name_box().text().equals("one.test.html"));
    files.close();
    CHECK(b.navigate("/notes.txt") && b.title().equals("notes.txt - Web Browser"));
    b.ask_save();
    CHECK(files.showing() && files.name_box().text().equals("notes.txt"));
    files.close();

    // Clearing the cookies cannot be undone: it asks, and Enter (the default) leaves everything as it was.
    CHECK(b.navigate("http://cookie.test/") && b.jar().count() == 1);
    CHECK(choose(w, bar, "Tools", "Clear Cookies and Site Data") && b.clear_dialog().showing() && b.clear_dialog().title().equals("Web Browser"));
    CHECK(b.clear_dialog().default_choice() == MessageDialog::Choice::Decline && b.clear_dialog().accept_button().text().equals("Clear") && b.clear_dialog().decline_button().text().equals("Cancel"));
    press(w, key::kEnter);
    CHECK(!b.clear_dialog().showing() && b.jar().count() == 1);
    CHECK(choose(w, bar, "Tools", "Clear Cookies and Site Data"));
    click_center(w, b.clear_dialog().accept_button());
    CHECK(!b.clear_dialog().showing() && b.jar().count() == 0 && b.status_label().text().equals("The cookies and data of all websites were cleared."));

    // The toggles show a check mark.
    CHECK(bar.find_item("Tools", "Run Scripts")->checked() && !bar.find_item("View", "Dark Colors")->checked() && !bar.find_item("Tools", "Block Third-Party Cookies")->checked());
    CHECK(choose(w, bar, "View", "Dark Colors") && bar.find_item("View", "Dark Colors")->checked() && b.dark());
    CHECK(choose(w, bar, "Tools", "Block Third-Party Cookies") && bar.find_item("Tools", "Block Third-Party Cookies")->checked() && b.block_third_party_cookies());

    // A page that asks before it is left: the question is titled by the page, and staying is the default.
    CHECK(b.navigate("http://leave.test/") && b.navigate("http://one.test/"));
    CHECK(b.leave_dialog().showing() && b.leave_dialog().title().equals("Leave - Web Browser") && b.leave_dialog().default_choice() == MessageDialog::Choice::Decline);
    press(w, key::kEnter);
    CHECK(!b.leave_dialog().showing() && b.address().equals("http://leave.test/"));
    CHECK(b.navigate("http://one.test/") && b.leave_dialog().showing());
    click_center(w, b.leave_dialog().accept_button());
    CHECK(b.address().equals("http://one.test/"));

    // While a field of the page is typed in, Edit works on it.
    CHECK(b.navigate("http://field.test/"));
    w.set_focus(&b.page());
    refresh_menus(w, bar);
    CHECK(item_enabled(bar, "Edit", "Select All") && item_enabled(bar, "Edit", "Copy") && item_enabled(bar, "Edit", "Paste"));
    CHECK(choose(w, bar, "Edit", "Select All") && choose(w, bar, "Edit", "Copy"));
    {
      String copied(a);
      CHECK(clipboard.get_text(&copied) && copied.view().equals("abc"));
    }

    // Help: F1 and Help > Help Topics show the help page (Back returns); About says what the program is.
    CHECK(b.navigate("http://one.test/"));
    w.set_focus(&b.go_button());
    press(w, key::kF1);
    CHECK(b.address().equals("about:help") && b.title().equals("Web Browser Help - Web Browser") && browser_text_op(b, "To keep a page or a file"));
    CHECK(b.go_back() && b.address().equals("http://one.test/"));
    CHECK(choose(w, bar, "Help", "Help Topics") && b.address().equals("about:help"));
    CHECK(choose(w, bar, "Help", "About Web Browser") && notices.showing() && notices.kind() == NoticeKind::Information && notices.title().equals("About Web Browser"));
    press(w, key::kEnter);
    CHECK(!notices.showing());

    // File > Close closes the frame; so does Ctrl+W.
    CHECK(b.manager() && choose(w, bar, "File", "Close") && !b.manager());
    CHECK(mgr.add_window(&b));
    w.layout();
    w.set_focus(&b.address_box());
    ctrl(w, 'w');
    CHECK(!b.manager());
  }
  CHECK(a.live == 0);

  // --- In the desktop, as a tab: New Tab from the menu, Ctrl+W closes the tab, the frame's keys reach it.
  TestAllocator a2;
  {
    filesystem::RamFileSystem fs(a2);
    Screen screen(a2, 1000, 700);
    desktop::Desktop d(a2, screen.renderer, nullptr, nullptr);
    d.set_file_system(&fs);
    BrowserGuideLoader loader;
    d.set_page_loader(&loader);
    Window& w = d.window();
    desktop::WebBrowser* b = d.open_web_browser();
    auto* host = static_cast<TabFrame*>(b->host());
    CHECK(host && host->as_tabs() && host->tab_count() == 1);
    host->set_bounds({20, 20, 800, 560});
    w.layout();
    CHECK(host->title().equals("New Tab - Web Browser"));
    refresh_menus(w, b->menu_bar());
    CHECK(item_enabled(b->menu_bar(), "File", "New Tab"));
    CHECK(choose(w, b->menu_bar(), "File", "New Tab") && host->tab_count() == 2);
    auto* second = static_cast<desktop::WebBrowser*>(host->tab_at(1));
    CHECK(second != b && host->selected() == second);
    w.layout();
    // Alt+A reaches the address box of the tab that is shown; Alt+Left / Alt+Right are still Back and Forward.
    w.set_focus(&second->page());
    alt(w, 'a');
    CHECK(second->address_box().focused());
    type_text(w, "one.test");
    press(w, key::kEnter);
    CHECK(second->address().equals("http://one.test/") && host->title().equals("Plain - Web Browser"));
    CHECK(second->navigate("http://one.test/next"));
    w.set_focus(&second->address_box());
    press(w, key::kLeft, modifier::kLeftAlt);
    CHECK(second->address().equals("http://one.test/"));
    press(w, key::kRight, modifier::kLeftAlt);
    CHECK(second->address().equals("http://one.test/next"));
    // Alt+F opens the File menu of the tab's own menu bar.
    alt(w, 'f');
    CHECK(second->menu_bar().open_menu_index() == 0);
    press(w, key::kEscape);
    // Ctrl+W closes the tab, not the window.
    w.set_focus(&second->address_box());
    ctrl(w, 'w');
    CHECK(host->tab_count() == 1 && host->selected() == b && host->manager());
  }
  CHECK(a2.live == 0);
}
