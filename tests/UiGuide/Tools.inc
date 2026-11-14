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
// The small applications: the calculator, the calendar, the to-do list, the terminal, the media player, the video viewer, the browser's
// Picture-in-Picture window and the 3D test.
#include "Desktop/Calculator.hpp"
#include "Desktop/Calendar.hpp"
#include "Desktop/Graphics3DTest.hpp"
#include "Desktop/MediaPlayer.hpp"
#include "Desktop/PictureInPicture.hpp"
#include "Desktop/Terminal.hpp"
#include "Desktop/Todo.hpp"
#include "Desktop/VideoViewer.hpp"
#include "Media/AudioCodecs.hpp"
#include "Media/AudioData.hpp"

static bool tools_item_enabled(MenuBar& bar, const char* menu, const char* item) {
  const MenuItem* i = bar.find_item(menu, item);
  return i && i->enabled();
}
// Opens and closes each menu of the bar, as a user looking through them does: the application brings its items up to date when a menu opens.
static void tools_look_through(Window& w, MenuBar& bar) {
  for (uint32_t m = 0; m < bar.menu_count(); ++m) {
    bar.open_menu(static_cast<int32_t>(m));
    if (w.popup()) w.close_popup();
  }
}
static std::string tools_clipboard(TestAllocator& a, kernel::Clipboard& cb) {
  String text(a);
  return cb.get_text(&text) ? std::string(text.view().data(), text.view().size()) : std::string();
}
// The last item of a menu that is not a separator.
static StringView tools_last_item(Menu& menu) {
  for (uint32_t i = menu.item_count(); i > 0; --i)
    if (!menu.item(i - 1)->separator) return menu.item(i - 1)->text.view();
  return StringView();
}
// No pixel of the area has this colour.
static bool tools_no_pixel(const Screen& screen, const Rect2D& area, uint32_t color) {
  for (int32_t y = area.y; y < area.y + static_cast<int32_t>(area.height); ++y)
    for (int32_t x = area.x; x < area.x + static_cast<int32_t>(area.width); ++x)
      if (screen.px(x, y) == color) return false;
  return true;
}

// A program for the terminal that, while it "runs", closes the terminal the way a user would and answers the question.
struct ToolsProgram final : desktop::IProgramLauncher {
  Window* window = nullptr;
  desktop::Terminal* terminal = nullptr;
  int interrupts = 0, runs = 0;
  struct Control final : desktop::IProgramControl {
    ToolsProgram* owner = nullptr;
    void send_input(StringView) override {}
    void end_input() override {}
    void interrupt() override { ++owner->interrupts; }
  } control;
  void run(const desktop::ProgramLaunch&, desktop::IProgramSink& sink, desktop::ProgramOutcome* outcome) override {
    ++runs;
    control.owner = this;
    sink.program_started(&control);
    CHECK(terminal->program_running());
    // Closing while the program runs asks first; the default button (Enter) and Escape both leave the program running.
    terminal->close();
    CHECK(terminal->manager() && terminal->question().showing() && terminal->question().default_choice() == MessageDialog::Choice::Decline);
    CHECK(window->focus() == &terminal->question().decline_button() && terminal->question().title().equals(terminal->title()));
    press(*window, key::kEnter);
    CHECK(!terminal->question().showing() && terminal->manager() && interrupts == 0);
    terminal->close();
    press(*window, key::kEscape);
    CHECK(!terminal->question().showing() && terminal->manager() && interrupts == 0);
    // "Close Terminal" stops the program and closes.
    terminal->close();
    CHECK(terminal->question().showing() && terminal->question().accept_button().text().equals("Close Terminal"));
    terminal->question().accept_button().click();
    CHECK(!terminal->manager() && interrupts == 1);
    sink.program_ended();
    outcome->kind = desktop::ProgramOutcome::Kind::Interrupted;
  }
};

struct ToolsAudioDevice final : hardware::IAudioDevice {
  uint32_t queued = 0;
  uint8_t vol = 80;
  bool mute = false;
  const char* name() const override { return "fake audio"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t write(ConstByteView pcm) override {
    uint32_t n = pcm.size() < 16384 - queued ? static_cast<uint32_t>(pcm.size()) : 16384 - queued;
    n -= n % hardware::kAudioFrameSize;
    queued += n;
    return n;
  }
  uint32_t queued_bytes() override { return queued; }
  uint32_t free_bytes() override { return 16384 - queued; }
  void set_volume(uint8_t v) override { vol = v; }
  uint8_t volume() const override { return vol; }
  void set_muted(bool m) override { mute = m; }
  bool muted() const override { return mute; }
  hardware::AudioStatistics statistics() override { return {}; }
};

struct ToolsVideoSources final : desktop::IVideoSources {
  uint32_t count = 1, watching = 0;
  uint32_t source_count() override { return count; }
  bool source(uint32_t index, uint32_t* id, String* name) override {
    if (index >= count) return false;
    *id = 7;
    return name->assign("Front Camera");
  }
  bool watch(uint32_t id) override { watching = id; return true; }
  bool latest(uint64_t*, List<uint32_t>*, uint32_t*, uint32_t*, StringView*) override { return false; }
};

static void ui_guide_tools() {
  // --- Calculator ----------------------------------------------------------------------------------------------------------------------------
  TestAllocator a;
  {
    kernel::Clipboard cb(a);
    Screen screen(a, 640, 480);
    Theme theme;
    Window w(a, screen.renderer, theme, {640, 480});
    w.set_clipboard(&cb);
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::Calculator c(a);
    CHECK(mgr.add_window(&c));
    w.layout();
    w.render();
    CHECK(c.title().equals("Calculator"));
    CHECK(standard_menu_order(c.menu_bar()) && menus_have_access_keys(c, c.menu_bar()) && help_menu_ends_in_about(c.menu_bar(), "Calculator"));
    // The focus starts in the display, where it is seen; the keys go to the number.
    CHECK(w.focus() == &c.display_control());
    type_text(w, "12+30");
    press(w, key::kEnter);
    CHECK(c.display().equals("42"));
    // Tab reaches the keypad. Enter is still "=" there (not a press of the button with the focus); Space presses that button.
    press(w, key::kTab);
    CHECK(w.focus() == c.key_button('C'));
    type_text(w, "*2");
    press(w, key::kEnter);
    CHECK(c.display().equals("84"));
    press(w, key::kSpace);
    CHECK(c.display().equals("0"));
    press(w, key::kTab, modifier::kLeftShift);
    CHECK(w.focus() == &c.display_control());
    // A division by zero is said in words, not "Error"; there is no number to copy then; Escape starts over.
    type_text(w, "5/0=");
    CHECK(c.display().equals("Cannot divide by zero"));
    tools_look_through(w, c.menu_bar());
    CHECK(!tools_item_enabled(c.menu_bar(), "Edit", "Copy"));
    press(w, key::kEscape);
    CHECK(c.display().equals("0") && tools_item_enabled(c.menu_bar(), "Edit", "Copy"));
    // Backspace, F9 (change sign) and the access keys of the lettered buttons.
    type_text(w, "75");
    press(w, key::kBackspace);
    press(w, static_cast<uint8_t>(key::kF1 + 8));
    CHECK(c.display().equals("-7"));
    // Ctrl+C copies the number shown; Ctrl+V takes a number (blanks around it do not count) as if it had been typed; other text is left alone.
    ctrl(w, 'c');
    CHECK(tools_clipboard(a, cb) == "-7");
    cb.set_text(" 2.5\n");
    ctrl(w, 'v');
    CHECK(c.display().equals("2.5"));
    type_text(w, "*2=");
    CHECK(c.display().equals("5"));
    cb.set_text("hello");
    ctrl(w, 'v');
    CHECK(c.display().equals("5"));
    // The same two commands are in the Edit menu and in the display's context menu (a shortcut is never the only way).
    CHECK(choose(w, c.menu_bar(), "Edit", "Copy") && tools_clipboard(a, cb) == "5");
    cb.set_text("8");
    const Point at = center(c.display_control());
    right_click(w, at.x, at.y);
    CHECK(w.popup() && click_popup_item(w, &c.display_menu().menu(), "Paste") && c.display().equals("8"));
    CHECK(choose(w, c.menu_bar(), "Edit", "Paste") && c.display().equals("8"));
    alt(w, 'c');   // the "C" button
    CHECK(c.display().equals("0"));
    type_text(w, "93");
    alt(w, 'b');   // "Back"
    CHECK(c.display().equals("9"));
    // With nothing on the clipboard Paste is disabled.
    cb.clear();
    tools_look_through(w, c.menu_bar());
    CHECK(!tools_item_enabled(c.menu_bar(), "Edit", "Paste"));
    // Help > About Calculator: an information message titled after the command.
    CHECK(choose(w, c.menu_bar(), "Help", "About Calculator") && c.about_dialog().showing() && c.about_dialog().title().equals("About Calculator"));
    CHECK(c.about_dialog().kind() == NoticeKind::Information);
    press(w, key::kEnter);
    CHECK(!c.about_dialog().showing());
    mgr.remove_window(&c);
  }
  CHECK(a.live == 0);

  // --- To-do list -----------------------------------------------------------------------------------------------------------------------------
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::TodoStore store(a, nullptr);
    desktop::Todo t(a);
    t.set_store(&store);
    CHECK(mgr.add_window(&t));
    w.layout();
    w.render();
    // Nothing to add, save, complete or delete yet: the buttons are disabled (not left to answer with a message).
    CHECK(!t.add_button().enabled() && !t.save_button().enabled() && !t.done_button().enabled() && !t.delete_button().enabled() && !t.clear_button().enabled());
    CHECK(t.clear_button().text().equals("Clear Completed") && t.done_button().text().equals("Mark Complete"));   // title capitalization
    // The label of the text field carries its access key: Alt+T goes there. Typing enables Add, which Enter presses.
    alt(w, 't');
    CHECK(w.focus() == &t.text_box());
    type_text(w, "Buy milk");
    CHECK(t.add_button().enabled() && t.add_button().is_default() && !t.save_button().is_default());
    press(w, key::kEnter);
    CHECK(t.item_count() == 1 && t.text_box().text().empty() && !t.add_button().enabled());
    type_text(w, "Call Ann");
    press(w, key::kEnter);
    CHECK(t.item_count() == 2);
    alt(w, 's');   // "Show:"
    CHECK(w.focus() == &t.show_box());
    // The list from the keyboard: the first arrow key chooses the first item; the form shows it and Enter in the form now saves.
    w.set_focus(&t.list());
    press(w, key::kDown);
    CHECK(t.list().selected_item() == 0 && t.text_box().text().equals("Buy milk") && t.save_button().is_default() && !t.add_button().is_default());
    // Delete asks first, and the button that keeps the item is the default: Enter and Escape change nothing.
    press(w, key::kDelete);
    CHECK(t.question().showing() && t.question().default_choice() == MessageDialog::Choice::Decline && w.focus() == &t.question().decline_button());
    CHECK(t.question().title().equals("To-Do") && t.question().accept_button().text().equals("Delete"));
    press(w, key::kEnter);
    CHECK(!t.question().showing() && t.item_count() == 2);
    t.delete_button().click();
    CHECK(t.question().showing());
    press(w, key::kEscape);
    CHECK(!t.question().showing() && t.item_count() == 2);
    t.delete_button().click();
    t.question().accept_button().click();
    CHECK(!t.question().showing() && t.item_count() == 1 && t.status().equals("Deleted."));
    // The item's menu from the keyboard (the Menu key): the move commands in title capitalization, and the edit command last.
    w.set_focus(&t.list());
    press(w, key::kDown);
    CHECK(t.selected_id() != 0);
    CHECK(tools_last_item(t.item_menu().menu()).equals("Edit") && t.item_menu().find_item("Move to This Week") && t.item_menu().find_item("Move to Next Week"));
    press(w, 0x65);
    CHECK(w.popup() && click_popup_item(w, &t.item_menu().menu(), "Move to This Week"));
    desktop::TodoStore::Item item(a);
    CHECK(store.get(t.selected_id(), &item) && item.bucket == desktop::Bucket::ThisWeek && t.status().equals("Moved to \"This week\"."));
    press(w, 0x65);
    CHECK(!click_popup_item(w, &t.item_menu().menu(), "Move to This Week"));   // (where it is already: disabled)
    w.set_focus(&t.list());
    press(w, 0x65);
    CHECK(click_popup_item(w, &t.item_menu().menu(), "Edit") && w.focus() == &t.text_box());
    // Enter in the list completes the item; clearing the completed ones asks first, too.
    w.set_focus(&t.list());
    press(w, key::kEnter);
    CHECK(t.item_count() == 0 && store.done_count() == 1 && t.clear_button().enabled());
    t.clear_button().click();
    CHECK(t.question().showing() && t.question().default_choice() == MessageDialog::Choice::Decline);
    press(w, key::kEnter);
    CHECK(store.done_count() == 1);
    t.clear_button().click();
    t.question().accept_button().click();
    CHECK(store.done_count() == 0 && !t.clear_button().enabled());
    mgr.remove_window(&t);
  }
  CHECK(a.live == 0);

  // --- Calendar -------------------------------------------------------------------------------------------------------------------------------
  {
    Screen screen(a, 900, 700);
    Theme theme;
    Window w(a, screen.renderer, theme, {900, 700});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::CalendarStore store(a, nullptr);
    desktop::Calendar c(a);
    c.set_store(&store);
    CHECK(mgr.add_window(&c));
    w.layout();
    c.select_date({2026, 11, 14});   // a Saturday
    w.render();
    // The month from the keyboard: arrows by day and week, Home and End to the ends of the week, Page Up / Down by month.
    w.set_focus(&c.grid());
    press(w, key::kLeft);
    CHECK(c.selected_date() == Date({2026, 11, 13}));
    press(w, 0x4A);   // Home
    CHECK(c.selected_date() == Date({2026, 11, 8}));
    press(w, 0x4D);   // End
    CHECK(c.selected_date() == Date({2026, 11, 14}));
    press(w, 0x4E);   // Page Down
    CHECK(c.selected_date() == Date({2026, 12, 14}));
    press(w, 0x4B);   // Page Up
    CHECK(c.selected_date() == Date({2026, 11, 14}));
    // The month marks its days with theme colours only (the appointment mark was a fixed blue).
    c.title_box().set_text("Dentist");
    CHECK(c.add_event() && c.event_count() == 1);
    w.render();
    const Point grid = c.grid().window_origin();
    CHECK(tools_no_pixel(screen, {grid.x, grid.y, desktop::MonthGrid::kWidth, desktop::MonthGrid::kHeight}, 0xFF2060C0));
    // The list has a label (it ends in a colon), and so has every field of the form.
    CHECK(c.day_label().text().equals("Appointments on 2026-11-14:"));
    // Nothing chosen: Save and Delete are disabled; with no title Add is, too. Typing a title makes Add the default, which Enter presses.
    CHECK(!c.update_button().enabled() && !c.delete_button().enabled() && !c.add_button().enabled());
    w.set_focus(&c.title_box());
    type_text(w, "Lunch");
    CHECK(c.add_button().enabled() && c.add_button().is_default());
    press(w, key::kEnter);
    CHECK(c.event_count() == 2);
    // A fault in the form keeps the form, says what is wrong and what to do, and puts the focus on the field at fault.
    w.set_focus(&c.grid());
    CHECK(!c.add_event() && w.focus() == &c.title_box() && c.status().equals("Give the appointment a title, then choose Add."));
    // An appointment chosen with the keyboard: Save is the default now; Delete asks, with Cancel as the default.
    w.set_focus(&c.list());
    press(w, key::kDown);
    CHECK(c.selected_event() != 0 && c.update_button().is_default() && !c.add_button().is_default() && c.delete_button().enabled());
    press(w, key::kDelete);
    CHECK(c.question().showing() && c.question().default_choice() == MessageDialog::Choice::Decline && w.focus() == &c.question().decline_button());
    CHECK(c.question().title().equals("Calendar"));
    press(w, key::kEnter);
    CHECK(!c.question().showing() && c.event_count() == 2);
    c.delete_button().click();
    press(w, key::kEscape);
    CHECK(c.event_count() == 2);
    // The appointment's menu ends in the edit command; the month has a menu too.
    CHECK(tools_last_item(c.event_menu().menu()).equals("Edit") && c.month_menu().find_item("Today"));
    w.set_focus(&c.list());
    press(w, 0x65);
    CHECK(w.popup() && click_popup_item(w, &c.event_menu().menu(), "Edit") && w.focus() == &c.title_box());
    w.set_focus(&c.list());
    press(w, 0x65);
    CHECK(click_popup_item(w, &c.event_menu().menu(), "Delete") && c.question().showing());
    c.question().accept_button().click();
    CHECK(c.event_count() == 1 && c.status().equals("Deleted."));
    mgr.remove_window(&c);
  }
  CHECK(a.live == 0);

  // --- Terminal -------------------------------------------------------------------------------------------------------------------------------
  {
    filesystem::RamFileSystem fs(a);
    CHECK(fs.create_directory("/bin") == filesystem::Status::Ok);
    {
      UniquePointer<filesystem::IFile> file;
      uint64_t written = 0;
      const char elf[] = "\x7F" "ELF\x02\x01\x01";
      CHECK(fs.open_file("/bin/tool", filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok &&
            file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(elf), sizeof elf - 1), &written) == filesystem::Status::Ok);
    }
    kernel::Clipboard cb(a);
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    w.set_clipboard(&cb);
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    NoticeDialog notices(a);
    desktop::Terminal t(a);
    t.set_file_system(&fs);
    t.set_notices(&notices);
    CHECK(mgr.add_window(&t));
    w.layout();
    w.render();
    MenuBar& bar = t.menu_bar();
    CHECK(standard_menu_order(bar) && menus_have_access_keys(t, bar) && help_menu_ends_in_about(bar, "Terminal"));
    // File ends in a separator and Exit (closing the frame ends the shell behind it).
    Menu* file = bar.menu(0);
    CHECK(file->title().equals("File") && file->item(file->item_count() - 1)->text.view().equals("Exit") && file->item(file->item_count() - 2)->separator);
    // Ctrl+C belongs to the running program, so copying is Ctrl+Shift+C: the menu says so, and both keys do what it says.
    CHECK(bar.find_item("Edit", "Copy All") && bar.find_item("Edit", "Copy All")->shortcut.view().equals("Ctrl+Shift+C"));
    t.view().clear();
    t.view().write("hello there\n");
    w.set_focus(&t.view());
    cb.set_text("before");
    ctrl(w, 'c');
    CHECK(tools_clipboard(a, cb) == "before");
    ctrl(w, 'c', modifier::kLeftShift);
    CHECK(tools_clipboard(a, cb).find("hello there") != std::string::npos);
    // Paste (Ctrl+V, the Edit menu, the view's context menu) puts the clipboard's line on the command line.
    cb.set_text("ls");
    ctrl(w, 'v');
    CHECK(t.view().input().equals("ls"));
    CHECK(choose(w, bar, "Edit", "Paste") && t.view().input().equals("lsls"));
    const Point at = center(t.view());
    right_click(w, at.x, at.y);
    CHECK(w.popup() && click_popup_item(w, &t.view_menu().menu(), "Paste") && t.view().input().equals("lslsls"));
    CHECK(t.view_menu().find_item("Copy All") && t.view_menu().find_item("Clear"));
    t.view().set_input(StringView());
    // With no file dialog there is nowhere to choose a file: Save Output As is disabled, not answered with a message.
    tools_look_through(w, bar);
    CHECK(!tools_item_enabled(bar, "File", "Save Output As...") && !choose(w, bar, "File", "Save Output As...") && !notices.showing());
    // A failed save says what happened to which file, why, and what to do; no path, no code.
    CHECK(!t.save_output("/nowhere/out.txt") && notices.showing() && notices.kind() == NoticeKind::Error);
    CHECK(notices.message().first(46).equals("The output could not be saved as \"out.txt\" bec") && notices.title().equals(t.title()));
    notices.close();
    // Help > About Terminal.
    CHECK(choose(w, bar, "Help", "About Terminal") && notices.showing() && notices.kind() == NoticeKind::Information && notices.title().equals("About Terminal"));
    notices.close();
    // Closing while a program runs asks first (see ToolsProgram); with nothing running the terminal just closes.
    ToolsProgram program;
    program.window = &w;
    program.terminal = &t;
    t.set_program_launcher(&program);
    t.run("/bin/tool");
    CHECK(program.runs == 1 && program.interrupts == 1 && !t.manager() && !t.program_running());
    desktop::Terminal second(a);
    second.set_file_system(&fs);
    CHECK(mgr.add_window(&second));
    w.layout();
    CHECK(choose(w, second.menu_bar(), "File", "Exit") && !second.manager() && !second.question().showing());
  }
  CHECK(a.live == 0);

  // --- Media player ---------------------------------------------------------------------------------------------------------------------------
  {
    filesystem::RamFileSystem fs(a);
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    NoticeDialog notices(a);
    ToolsAudioDevice device;
    audio::AudioMixer mixer(device, a);
    CHECK(mixer.initialize());
    {
      desktop::MediaPlayer mp(a);
      mp.set_notices(&notices);
      mp.set_services(&mixer, nullptr, nullptr);
      mp.set_file_system(&fs);
      CHECK(mgr.add_window(&mp));
      w.layout();
      w.render();
      MenuBar& bar = mp.menu_bar();
      CHECK(mp.title().equals("Media Player"));
      CHECK(standard_menu_order(bar) && menus_have_access_keys(mp, bar) && help_menu_ends_in_about(bar, "Media Player"));
      CHECK(bar.menu_count() == 3 && bar.menu(0)->title().equals("File") && bar.menu(1)->title().equals("Playback"));
      CHECK(tools_last_item(*bar.menu(0)).equals("Exit") && bar.find_item("File", "Open...") && bar.find_item("File", "Close"));
      // Nothing is open: what needs a file is disabled (and Open, with no file dialog to choose in).
      tools_look_through(w, bar);
      CHECK(!tools_item_enabled(bar, "File", "Close") && !tools_item_enabled(bar, "Playback", "Play") && !tools_item_enabled(bar, "Playback", "Stop"));
      CHECK(!tools_item_enabled(bar, "Playback", "Previous Track") && !tools_item_enabled(bar, "Playback", "Next Track") && !tools_item_enabled(bar, "File", "Open..."));
      press(w, key::kSpace);
      CHECK(!notices.showing());
      // A file that cannot be played: the message names it (no path), says why and what it is about.
      {
        UniquePointer<filesystem::IFile> file;
        uint64_t written = 0;
        CHECK(fs.open_file("/note.wav", filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok &&
              file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>("just some words, no sound"), 25), &written) == filesystem::Status::Ok);
      }
      CHECK(!mp.open_file("/note.wav") && notices.showing() && notices.kind() == NoticeKind::Error && notices.title().equals("Media Player"));
      CHECK(notices.message().equals("\"note.wav\" cannot be played because it is not a sound file and this machine cannot play video."));
      notices.close();
      CHECK(!mp.open_file("/gone.wav") && notices.message().equals("\"gone.wav\" could not be opened because it could not be found. It may have been moved, renamed or deleted."));
      notices.close();
      // Two sounds in one folder: the title is the file's name, then the program's; the commands come alive.
      media::AudioData tone(a);
      audio::StreamFormat format;
      format.sample_rate = 8000;
      format.channels = 1;
      CHECK(tone.create(format, 16000));
      for (uint64_t i = 0; i < 16000; ++i) tone.samples()[i] = static_cast<int16_t>((i % 80) * 300 - 12000);
      ByteBuffer wav(a);
      CHECK(media::encode_wav(tone, &wav));
      CHECK(fs.create_directory("/music") == filesystem::Status::Ok);
      for (const char* path : {"/music/a.wav", "/music/b.wav"}) {
        UniquePointer<filesystem::IFile> file;
        uint64_t written = 0;
        CHECK(fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok && file->write(0, wav.view(), &written) == filesystem::Status::Ok);
      }
      CHECK(mp.open_file("/music/a.wav") && !notices.showing() && mp.title().equals("a.wav - Media Player"));
      CHECK(mp.status_label().text().equals("Playing \"a.wav\" (1 of 2)"));
      tools_look_through(w, bar);
      CHECK(tools_item_enabled(bar, "File", "Close") && tools_item_enabled(bar, "Playback", "Pause") && tools_item_enabled(bar, "Playback", "Stop"));
      CHECK(tools_item_enabled(bar, "Playback", "Next Track") && tools_item_enabled(bar, "Playback", "Previous Track") && !bar.find_item("Playback", "Play"));
      // Space pauses and plays (the item is named after what it will do), and so does the menu; the context menu has the same commands.
      w.set_focus(&mp.audio_control()->seek_slider());
      CHECK(mp.playing());
      press(w, key::kSpace);
      CHECK(!mp.playing());
      tools_look_through(w, bar);
      CHECK(bar.find_item("Playback", "Play") && !bar.find_item("Playback", "Pause"));
      CHECK(choose(w, bar, "Playback", "Play") && mp.playing());
      w.layout();   // (the player's bar came with the file)
      const Point at = center(mp.audio_control()->position_label());
      right_click(w, at.x, at.y);
      CHECK(w.popup() && click_popup_item(w, &mp.playback_menu().menu(), "Pause") && !mp.playing());
      CHECK(choose(w, bar, "Playback", "Stop") && mp.audio_player()->state() == audio::PlayerState::Stopped);
      // The last track has no next one.
      press(w, key::kRight, modifier::kLeftControl);
      CHECK(mp.title().equals("b.wav - Media Player") && mp.status_label().text().equals("Playing \"b.wav\" (2 of 2)"));
      tools_look_through(w, bar);
      CHECK(!tools_item_enabled(bar, "Playback", "Next Track") && tools_item_enabled(bar, "Playback", "Previous Track"));
      CHECK(choose(w, bar, "Playback", "Previous Track") && mp.title().equals("a.wav - Media Player"));
      // File > Close lets go of the file; the frame stays.
      CHECK(choose(w, bar, "File", "Close") && mp.manager() && mp.kind() == desktop::MediaPlayer::Kind::None && mp.title().equals("Media Player"));
      tools_look_through(w, bar);
      CHECK(!tools_item_enabled(bar, "Playback", "Play") && !tools_item_enabled(bar, "File", "Close"));
      CHECK(choose(w, bar, "Help", "About Media Player") && notices.showing() && notices.title().equals("About Media Player"));
      notices.close();
      CHECK(choose(w, bar, "File", "Exit") && !mp.manager());
    }
  }
  CHECK(a.live == 0);

  // --- Video viewer ---------------------------------------------------------------------------------------------------------------------------
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    ToolsVideoSources sources;
    sources.count = 0;
    desktop::VideoViewer v(a);
    v.set_sources(&sources);
    CHECK(mgr.add_window(&v));
    w.layout();
    w.render();
    // With no source the line says so in a sentence, and what to do; the title is the program's name.
    v.update(1);
    CHECK(v.title().equals("Video Viewer") && v.status() == StringView("There are no video sources. Connect a camera or start sharing a screen."));
    // The list has a label whose access key goes to it.
    CHECK(v.source_label().text().equals("Source:"));
    alt(w, 's');
    CHECK(w.focus() == &v.source_list());
    // A source on show is named in the title, before the program.
    sources.count = 1;
    v.update(2000000);
    CHECK(sources.watching == 7 && v.title().equals("Front Camera - Video Viewer"));
    sources.count = 0;
    v.update(4000000);
    CHECK(v.title().equals("Video Viewer") && v.status() == StringView("The source is no longer there. Choose another one."));
    mgr.remove_window(&v);
    v.set_sources(nullptr);
  }
  CHECK(a.live == 0);

  // --- Picture-in-Picture ---------------------------------------------------------------------------------------------------------------------
  {
    Screen screen(a, 800, 600);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 600});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::PictureInPictureWindow pip(a);
    CHECK(pip.open(mgr, nullptr, 0, 320, 180));
    w.layout();
    w.render();
    // A palette: a title, only a Close button, no entry on the task bar.
    CHECK(pip.title().equals("Picture-in-Picture") && !pip.minimize_enabled() && !pip.maximize_enabled() && pip.close_enabled() && !pip.in_taskbar());
    // The picture takes the focus, so the keyboard reaches it; its menu has the click's command and Close.
    CHECK(pip.view().focusable() && w.focus() == &pip.view());
    press(w, key::kSpace);   // (no video here: nothing to play, and nothing goes wrong)
    CHECK(pip.picture_menu().find_item("Play") && tools_last_item(pip.picture_menu().menu()).equals("Close"));
    press(w, 0x65);
    CHECK(w.popup() && click_popup_item(w, &pip.picture_menu().menu(), "Close") && !pip.showing());
  }
  CHECK(a.live == 0);

  // --- 3D test --------------------------------------------------------------------------------------------------------------------------------
  {
    Screen screen(a, 800, 700);
    Theme theme;
    Window w(a, screen.renderer, theme, {800, 700});
    WindowManager mgr(a);
    mgr.set_dock(DockStyle::Fill);
    w.add(&mgr);
    desktop::Graphics3DTest test(a);
    CHECK(mgr.add_window(&test));
    w.layout();
    w.render();
    CHECK(test.title().equals("3D Test"));
    // With nothing to draw with, the view says so in the interface's colours: none of the old fixed ones (red text on the scene's dark blue).
    const Point origin = test.view().window_origin();
    const Rect2D area{origin.x, origin.y, test.view().bounds().width, test.view().bounds().height};
    CHECK(area.width > 100 && area.height > 100 && tools_no_pixel(screen, area, 0xFFFF5050) && tools_no_pixel(screen, area, 0xFF10101A));
    CHECK(screen.px(area.x + 2, area.y + 2) == theme.face);
    // The radio group has a label; the controls come in reading order for Tab (the API row, then the check boxes) and have access keys.
    CHECK(test.api_label().text().equals("Draw with:"));
    test.set_graphics(nullptr, nullptr, nullptr);
    test.texture_box().set_enabled(true);
    w.set_focus(&test.texture_box());
    CHECK(test.texture_box().checked());
    press(w, key::kSpace);
    CHECK(!test.texture_box().checked() && !test.view().use_texture());
    alt(w, 'l');   // "Logo texture"
    CHECK(test.texture_box().checked() && test.view().use_texture());
    Point api = test.api_button(desktop::CubeApi::Direct3D).window_origin(), boxes = test.texture_box().window_origin();
    CHECK(api.y < boxes.y);   // the API row is above the check boxes, and comes first in the tree
    mgr.remove_window(&test);
  }
  CHECK(a.live == 0);
}
