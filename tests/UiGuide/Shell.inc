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
// The desktop shell against the interface guidelines: the system keys (4.2), the launcher menu (8.4, 16), the Shut Down question and the windows with
// unsaved changes (6.3, 11.2, 11.8), the About window (11.1), the record of missed notifications (11.8), restored windows on a smaller screen (7.3).
#include "Database/Local/LocalDatabase.hpp"
#include "Database/Local/SystemSettings.hpp"
#include "Desktop/Desktop.hpp"
#include "Desktop/DesktopSurface.hpp"

static int32_t shell_menu_index(Menu& menu, const char* text) {
  for (uint32_t i = 0; i < menu.item_count(); ++i)
    if (!menu.item(i)->separator && menu.item(i)->text.view().equals(text)) return static_cast<int32_t>(i);
  return -1;
}
static constexpr uint8_t kShellF4 = key::kF1 + 3, kShellF6 = key::kF1 + 5;
// Types into a text editor window so that its document has unsaved changes.
static void shell_edit(desktop::Desktop& d, desktop::Notepad* n, const char* text) {
  n->activate();
  d.window().set_focus(&n->editor());
  type_text(d.window(), text);
}

static void ui_guide_shell() {
  // --- The launcher menu and the system keys.
  {
    TestAllocator a;
    {
      Screen screen(a, 1024, 768);
      desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
      Window& w = d.window();
      Menu& menu = d.start_button().menu();
      // About needs nothing more from the user: no ellipsis. Shut Down asks first: it has one, and it is last, after a separator.
      const int32_t about = shell_menu_index(menu, "About TheoryCraftOS");
      CHECK(about >= 0 && shell_menu_index(menu, "About TheoryCraftOS...") < 0);
      CHECK(shell_menu_index(menu, "Notifications") == about - 1);
      CHECK(shell_menu_index(menu, "Shut Down...") == static_cast<int32_t>(menu.item_count()) - 1 && menu.item(menu.item_count() - 2)->separator);
      for (uint32_t i = 0; i < menu.item_count(); ++i) {   // title capitalization, and "..." nowhere else
        const StringView text = menu.item(i)->text.view();
        if (menu.item(i)->separator) continue;
        CHECK(text[0] >= '0' && text[0] <= 'Z');
        CHECK(!text.ends_with("...") || text.equals("Shut Down..."));
      }
      // Ctrl+Esc opens the launcher; the arrow keys and Enter run an item; Ctrl+Esc again closes it. No pointer is needed for any of it.
      press(w, key::kEscape, modifier::kLeftControl);
      CHECK(d.start_button().open());
      press(w, key::kEscape, modifier::kLeftControl);
      CHECK(!d.start_button().open());
      press(w, key::kEscape, modifier::kLeftControl);
      press(w, key::kDown);
      press(w, key::kEnter);
      CHECK(!d.start_button().open() && d.explorer_count() == 1);
      Frame* files = d.explorer().host();
      desktop::Notepad* first = d.open_text_editor();
      desktop::Notepad* second = d.open_text_editor();
      CHECK(first && second && first->host() != second->host() && d.windows().active() == second->host());
      // Alt+F6: the next window of the same program, never the file explorer's.
      press(w, kShellF6, modifier::kLeftAlt);
      CHECK(d.windows().active() == first->host());
      press(w, kShellF6, modifier::kLeftAlt);
      CHECK(d.windows().active() == second->host());
      press(w, kShellF6, modifier::kLeftAlt | modifier::kLeftShift);
      CHECK(d.windows().active() == first->host());
      // Alt+Esc (like Alt+Tab): the next frame, whatever program it belongs to; with Shift the other way.
      Frame* before = d.windows().active();
      press(w, key::kEscape, modifier::kLeftAlt);
      CHECK(d.windows().active() != before && d.windows().active() != nullptr);
      press(w, key::kEscape, modifier::kLeftAlt | modifier::kLeftShift);
      CHECK(d.windows().active() == before);
      // Alt+F4 closes the active frame. One with nothing unsaved just closes.
      second->activate();
      press(w, kShellF4, modifier::kLeftAlt);
      CHECK(d.text_editor_count() == 1 && second->manager() == nullptr);
      // One with unsaved changes asks first, by every route the shell closes it: Alt+F4, the window's Close (the task-bar menu and the title bar's
      // button call the same Frame::close()), and the close button of its tab. Cancel leaves it as it was.
      shell_edit(d, first, "not saved");
      CHECK(first->editor().modified());
      press(w, kShellF4, modifier::kLeftAlt);
      CHECK(first->save_question().showing() && d.text_editor_count() == 1);
      press(w, key::kEscape);
      CHECK(!first->save_question().showing() && first->manager() && first->editor().text().equals("not saved"));
      first->host()->close();
      CHECK(first->save_question().showing() && first->manager());
      press(w, key::kEscape);
      TabFrame* tabs = first->host()->as_tabs();
      const Rect2D x = tabs->close_rect(0);
      const Point o = tabs->window_origin();
      click(w, o.x + x.x + static_cast<int32_t>(x.width) / 2, o.y + x.y + static_cast<int32_t>(x.height) / 2);
      CHECK(first->save_question().showing() && first->manager());
      click_center(w, first->save_question().alternate_button());   // Don't Save
      CHECK(d.text_editor_count() == 0 && first->manager() == nullptr);
      // The last frame, then Alt+F4 on the bare desktop: the Shut Down question.
      CHECK(d.windows().active() == files);
      press(w, kShellF4, modifier::kLeftAlt);
      CHECK(d.windows().window_count() == 0 && !d.shutdown_dialog().showing());
      press(w, kShellF4, modifier::kLeftAlt);
      CHECK(d.shutdown_dialog().showing() && d.shutdown_dialog().title().equals("Shut Down TheoryCraftOS"));
      // Shutting down cannot be taken back: Cancel is the default button, so Enter and Esc both leave everything as it was.
      CHECK(d.shutdown_dialog().default_choice() == MessageDialog::Choice::Decline && w.focus() == &d.shutdown_dialog().decline_button());
      press(w, key::kEnter);
      CHECK(!d.shutdown_dialog().showing() && !d.shutdown_requested() && !d.shutdown_pending());
      d.ask_shut_down();
      press(w, kShellF4, modifier::kLeftAlt);   // (Alt+F4 on the question is its close button: cancel)
      CHECK(!d.shutdown_dialog().showing() && !d.shutdown_requested());
      // Choosing Shut Down, with nothing unsaved anywhere, is the shutdown.
      d.ask_shut_down();
      click_center(w, d.shutdown_dialog().accept_button());
      CHECK(d.shutdown_requested() && !d.shutdown_pending());
    }
    CHECK(a.live == 0);
  }

  // --- Shutting down asks every window with unsaved changes first; Cancel in any of them calls the shutdown off.
  {
    TestAllocator a;
    {
      Screen screen(a, 1024, 768);
      desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
      Window& w = d.window();
      desktop::Notepad* one = d.open_text_editor();
      desktop::Notepad* two = d.open_text_editor();
      shell_edit(d, one, "first");
      shell_edit(d, two, "second");
      d.open_explorer();   // (in front: the window that is asked comes forward)
      d.ask_shut_down();
      click_center(w, d.shutdown_dialog().accept_button());
      CHECK(!d.shutdown_requested() && d.shutdown_pending());
      CHECK(one->save_question().showing() && !two->save_question().showing() && one->host()->state() != Frame::State::Minimized);
      press(w, key::kEscape);   // Cancel
      d.step(1000);
      CHECK(!d.shutdown_pending() && !d.shutdown_requested() && d.text_editor_count() == 2);
      CHECK(one->editor().text().equals("first") && two->editor().text().equals("second") && !two->save_question().showing());
      // Again: the first is thrown away, the second is asked next, and cancelling there keeps it (the first stays closed: that answer was given).
      d.ask_shut_down();
      click_center(w, d.shutdown_dialog().accept_button());
      CHECK(one->save_question().showing());
      click_center(w, one->save_question().alternate_button());
      d.step(2000);
      CHECK(one->manager() == nullptr && two->save_question().showing() && !d.shutdown_requested() && d.shutdown_pending());
      press(w, key::kEscape);
      d.step(3000);
      CHECK(!d.shutdown_pending() && !d.shutdown_requested() && two->manager() && two->editor().text().equals("second"));
      // And once more, with "Don't Save" for the one that is left: nothing unsaved remains, the system shuts down.
      d.ask_shut_down();
      click_center(w, d.shutdown_dialog().accept_button());
      CHECK(two->save_question().showing());
      click_center(w, two->save_question().alternate_button());
      d.step(4000);
      CHECK(d.shutdown_requested() && !d.shutdown_pending() && d.explorer_count() == 1);   // (windows with nothing to lose stay: the session remembers them)
    }
    CHECK(a.live == 0);
  }

  // --- The About window is a secondary frame: no task-bar entry, no minimize or maximize, OK is the default button, Esc closes too.
  {
    TestAllocator a;
    {
      Screen screen(a, 800, 600);
      desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
      Window& w = d.window();
      d.open_about();
      Frame& about = d.about_window();
      CHECK(about.manager() && about.title().equals("About TheoryCraftOS") && d.switcher().button_count() == 0);
      CHECK(!about.minimize_enabled() && !about.maximize_enabled() && about.close_enabled());
      press(w, key::kEnter);
      CHECK(about.manager() == nullptr);
      d.open_about();
      press(w, key::kEscape);
      CHECK(about.manager() == nullptr);
      d.open_about();
      d.open_about();   // one object, one frame
      CHECK(d.windows().window_count() == 1);
      press(w, kShellF4, modifier::kLeftAlt);
      CHECK(about.manager() == nullptr);
    }
    CHECK(a.live == 0);
  }

  // --- A notification that went away by itself is kept where it can be found: the Notifications window, announced by a button next to the clock.
  {
    TestAllocator a;
    {
      Screen screen(a, 1024, 768);
      desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
      Window& w = d.window();
      d.step(1000000);
      CHECK(!d.notifications_button().visible() && d.toasts().missed_count() == 0);
      CHECK(d.notify("Backup", "The backup has finished.", NoticeKind::Information, 1000) != 0);
      const uint32_t level = d.notify("Volume", "40%", NoticeKind::Information, 1000);
      d.toasts().set_transient(level);   // (feedback for a key: not a notification)
      const uint32_t taken_back = d.notify("Printer", "Out of paper.", NoticeKind::Warning, 1000);
      d.step(2000000);
      CHECK(d.toasts().count() == 3 && d.toasts().dismiss(taken_back));
      d.step(4000000);   // their time is up
      CHECK(d.toasts().count() == 0 && d.toasts().missed_count() == 1 && d.toasts().missed_title(0).equals("Backup"));
      d.step(4100000);
      CHECK(d.notifications_button().visible() && d.notifications_button().text().equals("Notifications: 1"));
      // The button opens the window (so does Start > Notifications, for the keyboard); having looked, the button goes.
      click_center(w, d.notifications_button());
      desktop::NotificationLog& log = d.notifications_window();
      CHECK(log.manager() && log.title().equals("Notifications") && log.grid().row_count() == 1 && d.switcher().button_count() == 0);
      CHECK(log.heading().ends_with(":") && !d.notifications_button().visible());
      // One that arrives while the window is open shows up in it.
      CHECK(d.notify("Download", "report.pdf was saved in Downloads.", NoticeKind::Information, 1000) != 0);
      d.step(5000000);
      d.step(7000000);
      d.step(7100000);
      CHECK(log.grid().row_count() == 2 && !d.notifications_button().visible());
      press(w, key::kEnter);   // Close is the default button
      CHECK(log.manager() == nullptr);
      press(w, key::kEscape, modifier::kLeftControl);
      CHECK(click_popup_item(w, &d.start_button().menu(), "Notifications") && log.manager());
      press(w, key::kEscape);
      CHECK(log.manager() == nullptr);
      CHECK(desktop::NotificationLog::kind_name(NoticeKind::Warning).equals("Warning"));   // the kind is a word, not only a colour
    }
    CHECK(a.live == 0);
  }

  // --- A window restored from the last session is adjusted to the screen there is now: all of it can be seen and reached.
  {
    TestAllocator a;
    {
      filesystem::RamFileSystem fs(a);
      filesystem::RamFileSystem system_files(a);
      database::local::LocalDatabase system_db(a, &system_files, "/AppStore/System/system.db");
      system_db.open();
      database::local::SystemSettings settings(a, system_db);
      {
        Screen screen(a, 1600, 1200);
        desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
        d.set_file_system(&fs);
        d.set_settings(&settings);
        desktop::Notepad* n = d.open_text_editor();
        n->host()->set_bounds({1000, 800, 560, 380});
        desktop::FileExplorer* e = d.open_explorer();
        e->host()->set_bounds({100, 60, 1300, 1000});   // larger than the next screen
        CHECK(d.save_session() == 2);
      }
      {
        Screen screen(a, 800, 600);
        desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
        d.set_file_system(&fs);
        d.set_settings(&settings);
        CHECK(d.restore_session() == 2);
        const Rect2D area = d.windows().area(0);
        for (Frame* f : {d.notepad().host(), d.explorer().host()}) {
          const Rect2D b = f->bounds();
          CHECK(b.x >= area.x && b.y >= area.y && b.x + static_cast<int32_t>(b.width) <= area.x + static_cast<int32_t>(area.width) &&
                b.y + static_cast<int32_t>(b.height) <= area.y + static_cast<int32_t>(area.height));
        }
        CHECK(d.notepad().host()->bounds().width == 560);   // (only moved: it fits)
      }
    }
    CHECK(a.live == 0);
  }

  // --- A message of the shell is titled with what it is about, and says what to do.
  {
    TestAllocator a;
    {
      filesystem::RamFileSystem fs(a);
      {
        UniquePointer<filesystem::IFile> file;
        uint64_t written = 0;
        const uint8_t elf[64] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
        CHECK(fs.open_file("/tool", filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok);
        CHECK(file->write(0, ConstByteView(elf, sizeof elf), &written) == filesystem::Status::Ok);
      }
      Screen screen(a, 800, 600);
      desktop::Desktop d(a, screen.renderer, nullptr, nullptr);
      d.set_file_system(&fs);
      CHECK(d.open_path("/tool"));
      NoticeDialog& notice = d.notice_dialog();
      CHECK(notice.showing() && notice.kind() == NoticeKind::Information && notice.title().equals("tool"));
      CHECK(notice.message().starts_with("\"tool\" is a program.") && notice.message().find("terminal") != StringView::npos);
      CHECK(notice.message().find("ELF") == StringView::npos && notice.message().find("rror") == StringView::npos);   // no internal names
      press(d.window(), key::kEnter);
      CHECK(!notice.showing() && d.text_editor_count() == 0);
    }
    CHECK(a.live == 0);
  }

  // --- The desktop's own painting takes its colours from the theme.
  {
    TestAllocator a;
    {
      Screen screen(a, 320, 200);
      Theme theme;
      theme.desktop = 0xFF102030;
      theme.desktop_text = 0xFFA0B0C0;
      Window w(a, screen.renderer, theme, {320, 200});
      desktop::DesktopSurface surface(a);
      surface.set_dock(DockStyle::Fill);
      w.add(&surface);
      w.layout();
      w.render();
      uint32_t marks = 0, others = 0;
      for (int32_t y = 0; y < 200; ++y)
        for (int32_t x = 0; x < 320; ++x) {
          const uint32_t p = screen.px(x, y);
          if (p == theme.desktop_text) ++marks;
          else if (p != theme.desktop) ++others;
        }
      CHECK(screen.px(0, 0) == theme.desktop && marks > 20 && others == 0);
    }
    CHECK(a.live == 0);
  }
}
